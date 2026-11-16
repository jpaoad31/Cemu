# Input bindings and settings rework (branch `input-rework`)

Modelled on Dolphin's input system (design only: Cemu is MPL-2.0, Dolphin GPL; all code here is our own).

## One device per controller (branch `input-single`, current model)

The user's decision: Dolphin's model. The device lists below ("Several devices per button", the per-device
rows) are **replaced** by this section; the expression engine, binding fields, Detect, the Advanced editor
and the mouse groups stay.

- **One selected device per emulated controller** (Dolphin's default device): a dropdown at the top of each
  controller tab ("None", Keyboard/Mouse, the Remote GamePad, the connected SDL/DSU pads and Wii devices,
  listed by a background thread; **Refresh**; **Other device...** for a DSU host or another Remote GamePad
  port), its state below it (connected / not found / waiting for the phone; red when not connected, "(not
  found)" in the list), then **Default bindings**, **Settings...**, **Calibrate**, **Clear all bindings**,
  **Load preset...**.
- **Keyboard/Mouse is one device** (API `Keyboard`, uuid `keyboard`, so old keyboard bindings stay valid). The
  mouse inputs are its inputs from id `0x40000000` (`MouseInputs::KeyboardId`), named `Mouse Left`, `Mouse
  Right`, `Middle`/`Back`/`Forward`, `Wheel Up/Down`, `Cursor X/Y`, `Cursor X+`..., `Move X+`..., with the
  `TV `/`GamePad ` prefixes (`GamePad Mouse Left`). The separate Mouse API is gone.
- **Plain bindings are the device's inputs**: written without a device, `` `X` | `Space` ``. Changing the
  device keeps the texts (Dolphin): `` `X` `` then means the new device's X; inputs it doesn't have show
  with `(?)`.
- **Changing the device** (my choice, Dolphin-like): with no bindings, its defaults are loaded at once; else
  a question "Load the default bindings of X?" (Yes replaces all bindings with X's defaults, default
  button when the API changes; No keeps them as text). **Default bindings** = replace all bindings with the
  device's defaults; for Keyboard/Mouse that is the preset keyboard layout (`InputPresets::KeyboardTable`,
  by key name) + the mouse's touch/tilt/pointer/balance.
- **Explicit cross-device inputs** stay possible in **Advanced...**: `` `RemoteGamePad/26770:A` ``,
  `` `Keyboard/keyboard:GamePad Mouse Left` ``. Such devices are attached on demand (created from the text,
  dropped when no binding names them) so they evaluate. Fields with them are drawn in the link colour with a
  mark at the left and the device in brackets (`GamePad Mouse Left [Keyboard/Mouse]`); a note counts them.
  The Advanced editor lists the device (inserted plain), the devices bindings already name and
  Keyboard/Mouse (inserted qualified); its Detect listens to all devices.
- **Detect listens only to the device.** "**Detect on all devices**" (checkbox, Dolphin's option) also
  listens to Keyboard/Mouse, connected SDL pads and the named devices; an input of another device is
  written qualified (explicit) and says so in the status line.
- **The device's own data**: motion (VPAD gyro/accel, the Nunchuk), the touch position, the Wii Remote's IR,
  rumble, battery, the Remote GamePad options (camera/mic/audio/video, shown while it is the device) come
  from the selected device only. With the Remote GamePad as the GamePad's device, the phone's touch and
  motion are the GamePad's; the mouse then touches only through explicit `Keyboard/keyboard:` inputs in the
  Touch group. Emulated tilt is used when the device gives no motion.
- **Balance Board source follows the device**: a Wiimote-API device -> the real board (a Wii Remote, or a
  board that isn't connected: the emulated board with the red fallback warning, as before); Keyboard/Mouse
  or a pad -> the emulated board.
- **Warnings**: "This device is also selected on Controller N" (an error for a Wii device / Remote GamePad,
  a note with the shared inputs for a keyboard or pad), a Balance Board on a non-board controller, the
  board fallback, the mouse cursor driving two controllers, bindings with `(?)`, explicit bindings.
- **Presets are "device + bindings"**: GamePad *Keyboard/Mouse*, *Remote GamePad (iPhone)*, *SDL pad* (its
  motion when it has a gyro; touch, and tilt without a gyro, from the mouse as explicit Keyboard/Mouse
  inputs); Wii Remote *Keyboard/Mouse pointer*, *Real Wii Remote*; board *Emulated: Keyboard/Mouse*, *Real
  board*; Pro/Classic *SDL gamepad*. "Keyboard + Remote GamePad" is gone.

### Migration of older profiles (bindings version < 3)

The user's decision: "just keep the bound device and drop the rest" (`input/emulated/SingleDeviceMigration.*`):
- Keyboard and the old Mouse device count as one device, Keyboard/Mouse (old Mouse names are renamed:
  `GamePad Left` -> `GamePad Mouse Left`). Profiles before version 2 first get the mouse's old implicit use as
  bindings, as before.
- The kept device is the **main non-keyboard device** if there is one (with several: the most inputs bound,
  then Remote GamePad > Wii device > others, then profile order), else Keyboard/Mouse.
- Plain bindings keep the kept device's inputs (written plainly) and **drop every other device's**.
  Advanced expressions (anything but a list) are kept whole, other devices explicit.
- Logged once per load (`controller profile ...: one device per controller: kept ..., dropped ...`) and shown
  in the tab (red): "Bindings from Keyboard/Mouse (12) were removed: one device per controller." Only the kept
  device is created (a dropped Remote GamePad doesn't start listening). The profile is written in the new
  format when the input settings close.

What the user's profiles (`controllerProfiles.20261115-232731`) become:

| profile | before | after |
|---|---|---|
| controller0 (GamePad) | Keyboard (no inputs) + Remote GamePad (25 bindings) + the mouse's implicit touch/tilt (12) | **Remote GamePad**, its 25 bindings; the 12 mouse bindings dropped (note shown); phone touch/motion |
| controller1 (Wii Remote) | Keyboard: Space -> 17 | **Keyboard/Mouse**, `` `Space` ``; nothing dropped |
| controller2 (Balance Board) | Keyboard: P power, 6 step-off + implicit mouse balance | **Keyboard/Mouse**, P, 6, balance `TV Cursor`; emulated board |
| (test) controller1 + a Wiimote | Keyboard Space + Wiimote A, B | **Wiimote 0**, `` `A` ``, `` `B` ``; Space dropped |
| (test) controller2 + a Wiimote (the white-screen case) | Keyboard P, 6 + Wiimote A | **Wiimote 0**, Power = `` `A` ``; P, 6 dropped; red: "Using the emulated board: the real board ... is not connected" and "also selected on Controller 2" |

### Profile format (version 3)

```xml
<controller><api>RemoteGamePad</api><uuid>26770</uuid>...<mappings>...</mappings></controller>
<bindings version="3">
  <binding id="1">`A`</binding>
  <binding id="104">`Keyboard/keyboard:GamePad Mouse Left`</binding>
</bindings>
```

Only the selected device is written as `<controller>` (with its old `<mappings>`, so older builds load it as a
one-device profile); explicitly named devices are recreated from the binding text. A version 3 profile with
several `<controller>` nodes uses the first. Older builds of this fork read version 3 bindings as errors
(unqualified inputs) and keep them as text.

### Expressions (version 3)

`` `Input` `` = the selected device's input; `` `API/uuid:Input` `` / `` `API:Input` `` = a given device. A text
before the first `:` is a device only if it looks like one (`Name` or `Name/uuid`); names with `:` are saved as
`#id`. A qualifier naming the selected device stays as written (explicit).

### Code (input-single)

| | |
|---|---|
| `input/emulated/SingleDeviceMigration.*` | old profile -> one device (pure, tested) |
| `input/emulated/InputExpression.*` | `DeviceSet::default_device`, `Leaf::qualified`, `remove_leaves`, `map_leaves`, `has_qualified_inputs` |
| `input/emulated/EmulatedController.*` | `get_device`/`set_device`, extra devices (`sync_devices_locked`), `load_default_bindings`; device data from the device only |
| `input/api/Keyboard/KeyboardController.*`, `input/api/Mouse/MouseInputs.h` | Keyboard/Mouse |
| `input/InputManager.cpp` | load (migration, notes), save (one `<controller>`) |
| `gui/wxgui/input/InputSettings2.*` | device dropdown, refresh thread, select/defaults question, note |
| `gui/wxgui/input/InputDetector.*` | device-only Detect, `candidate_devices`, all-devices option |

Tests: `InputExpressionTest.cpp` adds `test_default_device` (qualified/unqualified inputs, evaluation, the same
text against another device, `#id`, list editing), `test_keyboard_mouse_names` and `test_migration` (the
user's three profiles, the Wiimote variants, the choice of the main device, advanced expressions, unknown
devices). Detect's device filter is `InputDetector::candidate_devices` (wx code, not in the standalone test).
Scripted runs: copies of the user's profiles in a portable folder (see Testing), snapshots of the three tabs.

## What changed for users (input-rework; the device list is superseded, see above)

- **Several devices per button.** Every emulated control (a GamePad button, a stick direction, the
  board's Power button...) has a *list* of inputs, OR'ed together: `X [Keyboard] | A [Remote GamePad]`.
  Adding a device no longer steals buttons: it adds its default inputs only where it has none yet.
  The Remote GamePad "takes over all bindings" special case is gone.
- **Devices are listed per controller tab** with their state: *always available* (keyboard), *connected*,
  *not found*, *waiting for the phone*, *connected (Wii Remote)* / *connected (Balance Board)*. Per device:
  **Default bindings** (replaces only that device's inputs), **Settings...**, **Calibrate**, **Remove**.
  **Add device...** and **Clear all bindings** below the list. The old "Controller" box is gone.
- **Warnings** under the device list (and in the log when a game starts):
  - a real Wii device or Remote GamePad assigned to more than one emulated controller;
  - a keyboard key / pad input that drives two players;
  - a Balance Board on a non-board controller;
  - bindings that name a device that isn't attached (shown with `?`).
- **Balance Board source** is shown plainly at the bottom of the board tab: *Emulated board: ...*, *Real
  board: "name", connected* / *NOT connected*. If the assigned real device is a **Wii Remote** or **hasn't
  connected**, the **emulated board is used** (warning in red) instead of feeding nothing (Wii Fit U used to
  wait on a white screen for a calibration a Wii Remote never sends). When the real board connects, it takes
  over and the game is told the board reconnected, so it reads the real calibration. Once the real board has
  delivered data it stays the source; a dropout gives the game an empty board, as before.
- **Binding fields show live state**: highlighted while active, a bar for analogue values (stick
  directions, triggers). Stick indicators, motion bars (accel/gyro) and a touch-screen box on the GamePad tab.
- **Detect**: click a field, press an input on **any** device within 3 s (attached devices, the keyboard,
  any connected SDL gamepad; an unattached one is attached). **Esc** cancels, clicking the field again
  cancels. **Shift-click** (or Option/Command-click) **adds** the input instead of replacing.
- **Right click** a field: Detect, Detect and add, *Remove <input>* for each input, Clear, **Advanced...**
- **Advanced...**: the binding as an expression (syntax below), checked while you type, with its live value;
  pick inputs from a list per device, or Detect input.
- **macOS key names**: keys show as `A`, `Space`, `Left`, `Keypad 1`... (they were `key_0`, `key_49`...).
- The window **remembers its size and position**, scrolls when smaller than its content, and grows to fit a
  larger panel (within the screen).

## Mouse and pointer groups (branch `input-pointer`; the Mouse device is now part of Keyboard/Mouse, see above)

Nothing in the emulated controllers reads the mouse any more: the mouse is a **device** ("Mouse", always
available, listed with the others, Remove takes all its bindings away) and the controls it used to drive
implicitly are **binding groups**. Design after Dolphin (`ControllerInterface` cursor inputs, the Point/IR
group), our own code.

**Mouse inputs** (`input/api/Mouse/MouseInputs.h`, state in `input/MouseState.*`, fed by the game windows'
canvases via `wxgui/input/MouseFeed.h`). Each input exists for three windows: no prefix = the game window the
mouse is in (or was last in), `TV ` = the main window, `GamePad ` = the GamePad window when open, else the
main window.

| input | value |
|---|---|
| `Left` `Right` `Middle` `Back` `Forward` | pressed in that window (a click lasts at least 50 ms) |
| `Wheel Up` `Wheel Down` | pressed for 100 ms per notch |
| `Cursor X` `Cursor Y` | position over the window's image, -1..1, Y down |
| `Cursor X+` `X-` `Y+` `Y-` | the half axes, 0..1 (what the groups bind, like stick directions) |
| `Move X+` `X-` `Y+` `Y-` | relative motion (1 = the image width in half a second); `` `Mouse:Move X+` & `Mouse:Right` `` = only while dragging |

Only buttons and the wheel are "pressed", so **Detect** catches clicks and the wheel, not moves. While the
input settings window is open (it is modal: the game windows get no events), any click in it counts as the
no-prefix buttons and its wheel as the wheel, so Detect works there; a click in the window binds `Left`.
Window-specific inputs (`GamePad Left`, `TV Cursor X+`...) are picked in Advanced... or come from defaults
and presets.

**Groups and the Mouse device's defaults** (`input/emulated/MouseBindings.*`, control ids 100+ so they stay
out of the button loops):

| controller | group (controls) | Mouse default | other sources |
|---|---|---|---|
| GamePad | Touch screen (up/down/left/right, touch) | `GamePad Cursor` half axes + `GamePad Left` | a Remote GamePad's own touch wins while the phone is touched |
| GamePad | Emulated tilt (tilt up/down/left/right, turn left/right, hold to tilt) | `Cursor` half axes, `Wheel Up/Down`, hold = `Right` (the old right-drag tilt; 140 deg/s per turn input = 14 deg per notch) | a device with "Use motion" (SDL, DSU, Remote GamePad) wins |
| Wii Remote | Pointer (IR) (up/down/left/right, hide, recenter) + settings centre X/Y, width/height, relative, auto-hide (saved as `<pointer>`) | `TV Cursor` half axes; A = `TV Left`, B = `TV Right` | a real Wii Remote's IR wins while it sees the bar |
| Balance Board | Centre of balance (front/back/left/right) | `TV Cursor` half axes | none bound = centred (no more mouse fallback) |

Indicators: a pointer box per group (filled = touching / visible, hollow = not), the tilt's bars next to the
motion device's bars. Every group has a **Clear** button. A note under the device list says when the mouse
cursor drives two emulated controllers (e.g. GamePad tilt and board balance).

**Old profiles** (`<bindings>` missing or without `version="2"`) get today's implicit behaviour as explicit
bindings on load (the Mouse device is added): GamePad touch unless the touch group is bound, GamePad tilt
unless the tilt group is bound, the board's balance unless bound or a real Wii device is assigned. The Wii
Remote, Pro and Classic got nothing from the mouse before and get nothing now. Saved as `version="2"`;
older builds skip the "Mouse" device (unknown API) and ignore ids 100+. One difference: with a GamePad
window open, clicking the TV window no longer touches the GamePad screen (touch is "the GamePad view").

**Presets** (`input/InputPresets.cpp`, compiled in; **Load preset...** next to Add device): replace the
devices and bindings, then everything is editable.
- GamePad: *Keyboard + mouse* (A X, B Z, X C, Y V, L Q, R E, ZL 1, ZR 3, + Return, - Backspace, Home H,
  arrows D-pad, WASD left stick + F, IJKL right stick + G, Mic M, Show screen Tab; mouse touch + drag tilt),
  *Remote GamePad (iPhone)* (motion on), *Xbox / PlayStation / Switch Pro controller (SDL)* (first connected
  pad, motion on when it has a gyro, else mouse tilt; mouse touch), *Keyboard + Remote GamePad*.
- Wii Remote: *Keyboard + mouse pointer* (pointer = TV cursor, A TV click | X, B right click | Z, 1 2, = +,
  - -, H home, arrows, Nunchuk WASD/C/Shift, R recenter), *Real Wii Remote* (a connected one).
- Balance Board: *Emulated: keyboard + mouse* (P power, O step off, balance = TV cursor), *Real board*.
- Pro / Classic: *SDL gamepad*.

## Expressions

Inputs are written in backticks: `` `API/uuid:Input` `` (canonical, as saved) or `` `API:Input` `` (the first
device of that API). Input is the device's input name or `#id`. Examples:

    `Keyboard/keyboard:X` | `RemoteGamePad/26770:A`
    `SDLController/0_0300...:X-Axis+` * 1.5
    toggle(`Keyboard/keyboard:O`)
    `Keyboard/keyboard:Shift` & `Keyboard/keyboard:W`

| | value | active ("pressed") |
|---|---|---|
| input | 0..1 (axis directions: deflection) | the device reports it pressed (axes: past the dead zone) |
| `a \| b` | the stronger (larger magnitude) | either |
| `a & b` | the weaker | both |
| `!a` | 1 - a | not a |
| `a ^ b` | 1/0 | exactly one |
| `+ - * /`, `< >`, numbers | arithmetic | value > 0.5 |
| `min max clamp abs deadzone(x,dz)` | | value > 0.5 |
| `toggle(x)` `hold(x,s)` `pulse(x,s)` `timer(s)` | stateful: flip on press / after s seconds held / s seconds after a press / 0..1 sawtooth | |

Buttons use *active*; sticks and triggers use the value. Code: `src/input/emulated/InputExpression.{h,cpp}`
(no Cemu dependencies).

## Profile format

Controller profiles (`controllerProfiles/*.xml`) keep their devices (`<controller>` nodes, settings
unchanged) and get one new element:

```xml
<bindings>
  <binding id="1">`Keyboard/keyboard:X` | `RemoteGamePad/26770:A`</binding>
  <binding id="27">`RemoteGamePad/26770:Home`</binding>
</bindings>
```

`id` is the emulated control (`VPADController::ButtonId` etc.).

**Compatibility**
- Old profiles load: when `<bindings>` is missing, each device's old `<mappings>` entries become one input in
  the control's list. (Several devices mapping the same button used to be "last device wins"; now they're
  OR'ed, which is what you'd expect.)
- New profiles are still readable by older Cemu builds: every `<controller>` still gets its old `<mappings>`
  (the first input of that device per control). An older build loads them (with its one-device-per-button
  limit) and ignores `<bindings>`; if an older build *saves* the profile, `<bindings>` is dropped and the next
  load here rebuilds the lists from the old entries (expressions other than plain lists are lost then).
- A binding that doesn't parse is kept as text (inactive, logged) so nothing is lost on save.

## Code

| | |
|---|---|
| `input/emulated/InputExpression.*` | parser, evaluation, canonical printing, list editing |
| `input/emulated/InputProfileBindings.*` | `<bindings>` and old `<mappings>` read/write (pugixml only) |
| `input/emulated/InputExpressionTest.cpp` | standalone test: parser, evaluation, functions, list editing, old->new profile, Mouse inputs, implicit mouse -> bindings |
| `input/api/Mouse/*`, `input/MouseState.*`, `wxgui/input/MouseFeed.h` | the Mouse device |
| `input/emulated/MouseBindings.*` | group ids, Mouse defaults, old-profile conversion |
| `input/InputPresets.*` | Load preset... |
| `input/emulated/EmulatedController.*` | one expression per control; `set_mapping` (device's input), `add_mapping`, `replace_mapping`, `set_mapping_expression`, `set_default_mapping(device, replace)` |
| `*Controller.cpp` (VPAD, Pro, Classic, Wiimote, BalanceBoard) | `get_default_mapping()` (was `set_default_mapping`) |
| `input/emulated/BalanceBoardController.*` | `is_real_source`, `get_source_info`, fallback, reconnect on source change |
| `input/InputDiagnostics.*` | device status texts, conflict warnings, log at game start |
| `input/InputManager.cpp` | profile load/save |
| `gui/wxgui/input/InputSettings2.*` | the window: device list, warnings, geometry |
| `gui/wxgui/input/MappingControl.*`, `InputDetector.*`, `ExpressionDialog.*`, `LiveIndicators.*` | binding field, detect, advanced editor, motion/touch indicators |
| `gui/wxgui/input/panels/*` | grouped panels on the shared `InputPanel` |
| `gui/wxgui/wxWindowSystem.cpp` | macOS key names |

Wiimote / board / bridge internals were left alone apart from `BalanceBoardController` (source choice) and
`RealBoard::name`; the Wii Remote and Balance Board panels only swapped their text fields for binding fields.

## Testing

    P=build/vcpkg_installed/arm64-osx
    clang++ -std=c++20 -Wall -I src -I $P/include src/input/emulated/InputExpressionTest.cpp \
      src/input/emulated/InputExpression.cpp src/input/emulated/InputProfileBindings.cpp \
      src/input/emulated/MouseBindings.cpp src/input/emulated/SingleDeviceMigration.cpp \
      $P/lib/libpugixml.a -o /tmp/exprtest && /tmp/exprtest

Scripted window check with a **portable** data folder (a `portable/` directory next to the .app; never the
user's settings): `CEMU_TEST_INPUT_SETTINGS=SECONDS` opens Input settings at startup, closes it after SECONDS
(saving the profiles) and quits; `CEMU_TEST_INPUT_SETTINGS_PAGE=N` picks the tab;
`CEMU_TEST_INPUT_SETTINGS_SHOT=file.png` (macOS) saves a picture of the window first.

Verified that way: old-format profiles (keyboard + Remote GamePad on the GamePad, a Wii Remote device on both
a Wiimote and a Balance Board controller) load, show `X [Keyboard] | A [Remote GamePad ...]`, are saved with
`<bindings>` plus the old `<mappings>`; the board tab shows the conflict and the fallback to the emulated board.

Mouse groups, verified the same way: an old-format GamePad profile and an old board profile load with the
Mouse device and the touch/tilt and balance bindings (saved as `version="2"`); a Wiimote profile shows the
pointer group, its settings and the IR box; the board shows the centre-of-balance box; the cursor note.

Not verified (needs hands): mouse touch/tilt/pointer/balance in a game, Detect of clicks and the wheel, the
presets with real devices, Detect with real key presses and pads, the Advanced editor, Wii devices,
the board fallback during a Wii Fit U session, live indicators with real input.

## Left for later

- Hotkeys (`HotkeySettings`) still use their own one-key mapping.
- Emulated Wii Remote speaker output/volume in the UI (FORK_NOTES "for the input settings redo").
- A global "Wii devices" section for the bridge; per-device profiles (Dolphin's per-device default files).
- Expression editor niceties: syntax colouring.
- The raw WPAD read (not KPAD) still has no pointer data ("todo fill position api"), as upstream.
