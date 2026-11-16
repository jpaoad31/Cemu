# Input bindings and settings rework (branch `input-rework`)

Modelled on Dolphin's input system (design only: Cemu is MPL-2.0, Dolphin GPL; all code here is our own).

## What changed for users

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
| `input/emulated/InputExpressionTest.cpp` | standalone test: parser, evaluation, functions, list editing, old->new profile |
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
      $P/lib/libpugixml.a -o /tmp/exprtest && /tmp/exprtest

Scripted window check with a **portable** data folder (a `portable/` directory next to the .app; never the
user's settings): `CEMU_TEST_INPUT_SETTINGS=SECONDS` opens Input settings at startup, closes it after SECONDS
(saving the profiles) and quits; `CEMU_TEST_INPUT_SETTINGS_PAGE=N` picks the tab;
`CEMU_TEST_INPUT_SETTINGS_SHOT=file.png` (macOS) saves a picture of the window first.

Verified that way: old-format profiles (keyboard + Remote GamePad on the GamePad, a Wii Remote device on both
a Wiimote and a Balance Board controller) load, show `X [Keyboard] | A [Remote GamePad ...]`, are saved with
`<bindings>` plus the old `<mappings>`; the board tab shows the conflict and the fallback to the emulated board.

Not verified (needs hands): Detect with real key presses and pads, the Advanced editor, Wii devices,
the board fallback during a Wii Fit U session, live indicators with real input.

## Left for later

- Hotkeys (`HotkeySettings`) still use their own one-key mapping.
- Emulated Wii Remote speaker output/volume in the UI (FORK_NOTES "for the input settings redo").
- A global "Wii devices" section for the bridge; per-device profiles (Dolphin's per-device default files).
- Expression editor niceties: syntax colouring, a mouse axis input.
