# Emulated Balance Board (branch `wii-fit-u`)

This branch adds an emulated **Wii Balance Board** to Cemu, so Wii Fit U can be played on a Mac
without a real board. It is a new emulated controller type, **Balance Board**, plus the padscore
HLE that Wii Fit U uses to talk to the board (WPAD BLC calls and the WBC library).

## Where the code comes from

- **Load cell maths, calibration block, CRC, temperature, battery:** adapted from Dolphin's
  emulated balance board, upstream PR
  [dolphin-emu/dolphin#13337](https://github.com/dolphin-emu/dolphin/pull/13337)
  (GPL-2.0-or-later, like Cemu), as used in `~/github/dolphin-bb` (branch `balance-board-emu`):
  the same 10000/11700/13400 raw calibration at 0/17/34 kg on every load cell, reference
  temperature 0x19, the calibration CRC-32, the centre-of-balance to sensor split, foot
  separation/length scaling and the battery fit. The Step Off toggle/hold hotkeys and the 100%
  battery default come from our Dolphin branch.
- **WBC library (raw -> kg, zero point, TGC weight, battery level):** a clean implementation of
  the behaviour: linear interpolation between each load cell's three calibration points,
  relative to the zero point the game measures, a -0.7% per 10 temperature units correction and a
  per-region gravity factor. The decompiled Wii SDK WBC in `~/github/jpaoad31/wii-fit-plus` was
  only read to learn the behaviour and to check results; no code was copied. Return values and
  argument order were taken from Wii Fit U's own calls (`build/rpx/step.dis` + relocations).

Files:

| File | What |
| --- | --- |
| `src/input/emulated/BalanceBoardMath.{h,cpp}` | calibration memory, CRC, kg <-> raw, centre of balance, temperature, battery (standard headers only) |
| `src/input/emulated/BalanceBoardMathTest.cpp` | standalone check of the maths (not in CMake; Cemu has no test target) |
| `src/input/emulated/BalanceBoardController.{h,cpp}` | the `BalanceBoardController` (a `WPADController`): mappings, settings, profile load/save |
| `src/input/emulated/WPADController.*`, `EmulatedController.*`, `ControllerFactory.cpp` | dev type 3, data format 12 (`WPADBLStatus`), type `Balance Board` |
| `src/input/InputManager.cpp` | the board always gets WPAD channel 3 |
| `src/Cafe/OS/libs/padscore/padscore.{h,cpp}` | `WPADEnableWBC`, `WPADRegisterBLCWorkarea`, `WPADIsRegisteredBLC`, `WPADControlBLC`, `WPADGetBLCalibration`, `WBCSetupCalibration`, `WBCGetCalibrationStatus`, `WBCSetZEROPoint`, `WBCRead`, `WBCGetTGCWeight`, `WBCGetBatteryLevel`; `WPADGetInfo(Async)` battery/extension for the board; KPAD `ex_status.bl` |
| `src/gui/wxgui/input/panels/BalanceBoardInputPanel.*`, `InputSettings2.cpp` | the settings panel and the "Balance Board" entry |

## What Wii Fit U does with the board (from the disassembly)

- It looks for the board on **WPAD channel 3 only** (`RPHealthMeasure` reads `WPADRead(3)`,
  `WPADControlBLC(3, 0, NULL)`, `WPADGetBLCalibration(3, buf, 0x24, 16, cb)`, and checks
  `WPADProbe(3)` dev type == 3). Non-board devices on channels 2 and 3 (and 1, unless a game flag allows a second remote) get `WPADDisconnect`ed.
- When dev type 3 appears it calls `WBCSetupCalibration()`, later polls
  `WBCGetCalibrationStatus()`, reads the 0 kg points with `WPADGetBLCalibration`, waits for a
  valid temperature, averages 120 raw samples with nobody on the board and passes them to
  `WBCSetZEROPoint(zero, 4)`. Weights come from `WBCRead(&status, weights, 4)` (1 = someone on
  the board, 0 = nobody, -2 = not calibrated) and `WBCGetTGCWeight(&out, &status, sum)`.
- The game never calls `WPADSetDataFormat`, so the board reports format 12 from the start.

## Building

```sh
cd ~/github/cemu-bb
cmake --build build > build.log 2>&1      # already configured: Release, MACOS_BUNDLE, Metal
# maths check:
clang++ -std=c++20 -Wall -I src src/input/emulated/BalanceBoardMathTest.cpp \
  src/input/emulated/BalanceBoardMath.cpp -o /tmp/bbtest && /tmp/bbtest
```

The app is `bin/Cemu_release.app`.

## Using it

1. **Options > Input settings.** Keep **Controller 1 = Wii U GamePad** (Wii Fit U needs it).
   Set **Controller 2 = Wiimote** (keyboard or a real remote) for the menus.
2. Pick another tab (e.g. **Controller 3**), set **Emulated controller = Balance Board**, then add
   the **Keyboard** as its controller (default mappings are filled in), or use **Load** with the
   profile `balanceboard`, which is in `~/Library/Application Support/Cemu/controllerProfiles/`.
   Whatever tab you use, the board is always WPAD channel 3 (Wii Remote 4); a Wiimote on that
   channel is moved to the next free one. Only one Balance Board can be configured.
3. Mappings (keyboard defaults):
   - **Balance** (centre of balance): **unmapped by default, so it follows the mouse.** The mouse
     position over the TV image in the main window is the centre of balance, measured the same way
     as the GamePad touch screen: the image centre is centred (weight spread evenly), its edges are
     a full lean (left/right = left/right, top = front, bottom = back). No click needed. Moving the
     mouse out of the image clamps to the edge.
     To override it, map Balance front/back/left/right to keys or a gamepad stick (SDL controllers
     get the left stick by default); as soon as any of the four is mapped, the mapping is used and
     the mouse is ignored.
   - **Power** = **P**: the board's front button (reported as A).
   - **Step off (toggle)** = **O**: off the board (0 kg) / back on. **Step off (hold)**: unmapped
     by default, off the board while held.
   - **Options:** Weight (default 60 kg), Foot separation (27 cm) and Foot length (23 cm): how far
     a full stick deflection moves the centre of balance, Battery (100%).
   - The bottom line of the panel shows the live load (total and per sensor) and the step-off state.
4. In Wii Fit U, when it asks you to **step off** the board (zero point): press **O** and wait.
   When it asks you to **step on**: press **O** again and keep the mouse in the middle of the image (or the Balance stick centred) and still
   until the weight is measured. The game should show about 59.9 kg for Weight = 60 (the WBC
   gravity factor, 0.99908 for USA/JPN, as on a real board).
5. For testing, enable **Debug > Logging > Input API**. Lines starting with `BalanceBoard:` show
   the channel assignment, the calibration CRC and points, every `WPADGetBLCalibration` read
   (address, length, bytes), `WPADControlBLC`, `WBCSetZEROPoint` values, the first `WBCRead`s and
   every change of on/off the board, and the first `WBCGetTGCWeight` results. The log is
   `~/Library/Application Support/Cemu/log.txt`. (Input API logging is very verbose.)

## What was verified

- The branch builds with no warnings in the new or changed files.
- `BalanceBoardMathTest`: identifier bytes, calibration bytes, CRC (`dc710d7d`) against an
  independent table CRC and Python's `zlib.crc32`, CRC mismatch detection, kg -> raw -> kg round
  trip, 60 kg centred = 11500 raw per cell = 6000 load = on the board, step off = 0, 100 kg uses
  the second segment, centre of balance round trip, temperature and battery levels. The raw -> kg
  results were also compared (in a scratch copy) with the Wii SDK's slope/intercept form and match.
- Boot of Wii Fit U (USA, Rev 2) with GamePad + Wiimote + Balance Board on controller 3, in a
  throwaway portable profile: the log shows `controller 3 is the Balance Board on WPAD channel
  3`, `WPADRegisterBLCWorkarea`, `WPADEnableWBC`, the connect callback for channel 3, `WPADProbe(3)`
  each frame, `WBCSetupCalibration -> 1` with `CRC dc710d7d/dc710d7d ok`, and
  `WPADIsRegisteredBLC -> 1`. The game kept running to its menus (GamePad input needed from there).

## Not verified

- The weigh-in / body test / balance games: they need the menus, so they have to be played by
  hand. `WPADGetBLCalibration`, `WPADControlBLC`, `WBCSetZEROPoint`, `WBCRead` and
  `WBCGetTGCWeight` were not reached in the scripted boot.
- How the game's zero point flow behaves on the first run (it may reuse a saved zero point,
  `SpecialMove`), and whether Wii U's WBC gravity factors and battery thresholds equal the Wii's.
- The settings panel was built but not opened.
- The KPAD `ex_status.bl` layout is the Wii SDK's; Wii Fit U is not known to read it.

# Real board (branch `real-balance-board`; tested in Wii Fit U through the Wii BT Bridge)

A real Wii Balance Board can feed the same Balance Board controller: the game then gets the
board's **own raw load cells, temperature and battery** and **its own calibration block** through
`WPADRead` (format 12), `WPADGetBLCalibration` and the WBC functions. Nothing is converted to kg
and back; the WBC HLE converts the raw values with the real calibration, like on a console.

## How it works

- **Detection** (`WiimoteControllerProvider`, hidapi path): the board is a Wii Remote, VID 057e /
  PID 0306, HID name `Nintendo RVL-WBC-01`, with a permanently attached extension
  `00 00 a4 20 04 02`. Cemu's Wiimote provider finds it like any Wii Remote (it rescans every 2 s).
  The name marks it as a board at once; the extension ID confirms it (the ID constant was wrong
  before: 0x2A2C).
- **Initialisation**, as Dolphin does (`WiimoteReal.cpp` `IsBalanceBoard`): status request, then
  0x55 to 0xA400F0 and 0x00 to 0xA400FB (unencrypted extension), read the ID at 0xA400FA, then
  read the calibration 0xA40020..0x3F (two 16-byte replies) and 0xA40060..61 (reference
  temperature, version) and check the CRC (`BalanceBoardMath::ParseCalibration`). No IR camera,
  accelerometer calibration or MotionPlus probing for a board. Report mode **0x34**, continuous.
  Missing calibration parts are re-read on the next status report. LED 1 (the blue power light)
  is lit while Cemu holds the board.
- **Reports**: 0x32/0x34/0x35/0x3D extension bytes: TR, BR, TL, BL (u16 big-endian), temperature,
  padding, battery. The power button is the board's A button.
- **To the game**: `BalanceBoardController` uses a real board when a controller with API
  **Wiimote** is assigned to it; otherwise it stays the emulated board (unchanged behaviour).
  `fill_balance_status` passes the raw values, `get_extension_memory` the real calibration
  memory, `get_battery_raw` the board's battery byte. `WBCSetupCalibration` waits (via
  `WBCGetCalibrationStatus`) until the real calibration has been read. `WPADControlBLC` commands
  are forwarded to the board's register 0xA400F1. If the board drops out, the game sees an empty
  board (its 0 kg points) instead of a frozen weight.

Files: `src/input/api/Wiimote/{WiimoteMessages.h, WiimoteDevice.h, WiimoteControllerProvider.*,
NativeWiimoteController.*, hidapi/HidapiWiimote.*}`, `src/input/emulated/BalanceBoardController.*`,
`src/Cafe/OS/libs/padscore/padscore.cpp`, `src/gui/wxgui/input/panels/BalanceBoardInputPanel.cpp`.
The protocol (IDs, registers, init writes, report layout, calibration layout and CRC) comes from
Dolphin (GPL-2.0-or-later): `WiimoteReal.cpp`, `IOhidapi.cpp`, `WiimoteEmu/Extension/BalanceBoard.cpp`.
The `WPADControlBLC` register/byte sequence was learned from the Wii SDK's behaviour (decomp read
only, no code copied).

## Through the Wii BT Bridge (recommended on macOS)

macOS can't connect Wii devices at all (it answers their PIN request with a fixed "0000"; see
`docs/macos-findings.md` in [wii-bt-bridge](https://github.com/jpaoad31/wii-bt-bridge)). The Wii BT
Bridge does the Bluetooth part instead: a Pico W on USB (it shows up as a USB network adapter, the
bridge is `192.168.7.1:4730`) or a Raspberry Pi. It pairs the devices with the right PIN, takes the
board back when its power button is pressed, and forwards the raw HID reports over UDP. Cemu's
real-board code (extension init, calibration + CRC, report 0x34, WPADControlBLC, battery) runs on top
unchanged.

- **Backend:** `src/input/api/Wiimote/bridge/WiiBridgeWiimote.{h,cpp}` (portable, boost.asio UDP). One
  session thread: HELLO every 500 ms (the bridge's keepalive), HELLO_ACK / CONNECTED / DISCONNECTED /
  DATA_IN / LOG. Every connected bridge slot is a `WiimoteDevice`; `read_data` returns the bridge's
  DATA_IN reports (report ID first, like hidapi), `write_data` sends DATA_OUT (the bridge adds 0xA2).
  Devices are identified by Bluetooth address (`get_identity()`), not by slot. If the bridge stops
  answering for 3 s its devices disconnect; when it answers again it replays CONNECTED and they come back.
- **Device indexes stay put:** controller profiles refer to Wiimotes by index (`uuid` 0, 1, ...).
  `WiimoteControllerProvider` now remembers which device (`WiimoteDevice::get_identity()`) last used
  each index and gives a returning device its old index, and doesn't hand that index to another
  device while the owner is among the devices found. Before, the lowest free index went to whichever
  device was found first, so a Wii Remote could take the board's index after a reconnect.
- **Settings:** `settings.xml`, `<Input><WiiBridge enabled="true" host="192.168.7.1" port="4730"/></Input>`,
  or on the Balance Board panel: **Wii BT Bridge at** (checkbox + address), **Scan with bridge** (asks the
  bridge to look for devices in sync mode for 30 s), **Stop**, and a status line (bridge answering,
  firmware, connected devices). The environment variable `CEMU_WII_BRIDGE=host[:port]` overrides the
  settings and enables the bridge (for testing without the UI).

### Connecting through the bridge

1. Plug in the bridge (or start it). Enable **Debug > Logging > Input API** to see the steps.
2. **Options > Input settings**, the Balance Board tab: tick **Wii BT Bridge at** (default address).
3. First time: **Scan with bridge**, then press the red **SYNC** button on the board. After that, the
   board's **power button** connects it again, no scan needed (the bridge remembers it; the board goes
   in the bridge's slot 3, Wii Remotes elsewhere).
4. API **Wiimote**, refresh, choose the board's entry and add it, as in "Setting it up in Cemu".

### What the log shows (bridge part)

```
WiiBridge: session started, bridge at 192.168.7.1:4730 (local port ...)
WiiBridge: bridge 28:CD:C1:04:14:41 answering: protocol 1, firmware 10, 7 slots
WiiBridge: slot 3 connected: "Nintendo RVL-WBC-01" (34:AF:2C:E6:C1:87)
Wiimote: found device 0 "Nintendo RVL-WBC-01" at bridge 192.168.7.1:4730 slot 3 (34:AF:2C:E6:C1:87) (Balance Board by its name)
... then the BalanceBoard(real) lines below (calibration, CRC ok, reports)
WiiBridge: slot 3 disconnected: "Nintendo RVL-WBC-01" (34:AF:2C:E6:C1:87), reason 0x16
WiiBridge: the bridge at 192.168.7.1:4730 stopped answering
WiiBridge: bridge log: ...        (the bridge's own log lines)
```

### Verified (without hardware, with the bridge repo's `tools/fake_bridge.py`)

The fake bridge answers like the Pico with the real board (a dump of its extension memory, so the
calibration CRC is real). With `CEMU_WII_BRIDGE=127.0.0.1:4731` and a portable profile whose Balance
Board controller has Wiimote device 0, booting Wii Fit U: the board was found as device 0, the
calibration read with `CRC c99baa41 stored / c99baa41 computed: ok`, `source is the real board ...
ready`, the game's `WBCSetupCalibration()` got the real calibration, and report #6000 converted to
17.09 + 15.30 + 14.57 + 13.05 = 60.0 kg (the fake board's weight), about 90 reports/s. A board
disconnect / reconnect (2.6 s to ready again) and a bridge restart (5 s) recovered; with a Wii Remote
added after the restart the board kept device 0. Not verified: the Balance Board panel's bridge
controls (built, not opened), the Scan button (needs the UI), a real bridge and board, the weigh-in.

## Real board on macOS (IOBluetooth, no pairing)

**Doesn't work (tested on hardware, November 2026):** macOS opens its own HID connection to the device
and answers the Wii's PIN request with a fixed "0000", so the board fails authentication whatever
Cemu does. Use the Wii BT Bridge (above). The section below is kept for reference.

macOS 27.2 can't pair the board: `blueutil --inquiry` sees "Nintendo RVL-WBC-01"
(34-af-2c-e6-c1-87) in sync mode, but `--pair` fails with 0x1f (the Wii binary-PIN problem) and
`--connect` times out, so the hidapi path never gets a device. Wii devices in sync mode accept
direct L2CAP connections on PSM 0x11 (control) and 0x13 (interrupt) without pairing, so Cemu now
connects them itself.

- **Backend:** `src/input/api/Wiimote/darwin/IOBluetoothWiimote.{h,mm}` (Objective-C++, APPLE only,
  `ENABLE_IOBLUETOOTH`, links IOBluetooth + CoreBluetooth). Adapted from Dolphin's removed
  `IOdarwin.mm` (commit e9a11cdbfd, GPL-2.0-or-later). A `WiimoteDevice` like the hidapi one: writes
  go to the interrupt channel with the 0xA2 header (`writeAsync`), interrupt data (0xA1 stripped)
  goes into a thread-safe queue that `read_data` drains. All IOBluetooth calls and callbacks run
  on the main thread (main dispatch queue); the steps are asynchronous, so the UI doesn't block.
- **No constant scanning** (Dolphin's CPU problem): an inquiry runs only after the button click,
  in rounds of 10 s, for **up to 60 s**, and ends as soon as a Nintendo RVL device
  (`Nintendo RVL-CNT-01*` except `-UC`, `Nintendo RVL-WBC*`) is found **and connected**. Finding one
  stops the inquiry (paging during an inquiry is unreliable), connects it, and resumes the
  inquiry only if the connection failed and time is left. Connected devices stay open (also
  across input provider restarts) until the device disconnects.
- **Connect steps:** baseband (`openConnection:withPageTimeout:authenticationRequired:NO`, 5 s page
  timeout), then `openL2CAPChannelAsync` PSM 0x11, then PSM 0x13; 20 s overall timeout.
- **Provider:** `WiimoteControllerProvider::connectionThread` lists the IOBluetooth devices first;
  `HidapiWiimote::get_devices` skips a HID device whose serial number (macOS uses the Bluetooth
  address) belongs to an IOBluetooth connection, so a device isn't opened twice. The hidapi path
  stays for setups where pairing works. From there everything is as above (board init,
  calibration, report 0x34).
- **Permission:** `NSBluetoothAlwaysUsageDescription` in `MacOSXBundleInfo.plist.in`. The access
  state (`CBManager.authorization`) is logged at each scan; denied/restricted stops with a log line
  and a panel message; "not determined" triggers the system prompt (click again after allowing).

### Connecting

1. Batteries in the board. Enable **Debug > Logging > Input API**.
2. **Options > Input settings**, the Balance Board controller tab. Click **Connect real Wii device**,
   then press the **red sync button** in the battery compartment (the blue light blinks). You have
   up to 60 s; **Cancel** stops the scan. The line next to the buttons shows "Scanning... N s left",
   then "Found ..., connecting...", then "Connected: Nintendo RVL-WBC-01 (34-af-2c-e6-c1-87)".
3. Pick API **Wiimote**, refresh, choose the board's entry and add it (as in "Setting it up in Cemu").
   The blue light turns steady (LED 1) once the provider has initialised it.
4. After the board powers off (or Cemu quits), connect again the same way.

### What the log shows (IOBluetooth part)

```
IOBluetooth: host controller 60:3E:5F:34:3E:A1, access allowed
IOBluetooth: scan started (up to 60 s, ends at the first Nintendo RVL device connected)
IOBluetooth: found device 34-af-2c-e6-c1-87 "Nintendo RVL-WBC-01" (class of device 0x......): a Nintendo RVL device
IOBluetooth: 34-af-2c-e6-c1-87 "Nintendo RVL-WBC-01": opening the baseband connection (no pairing, no authentication)
IOBluetooth: 34-af-2c-e6-c1-87 baseband connection open (0x00000000), opening L2CAP PSM 0x11 (control)
IOBluetooth: 34-af-2c-e6-c1-87 L2CAP PSM 0x11 (control) open, opening PSM 0x13 (interrupt)
IOBluetooth: 34-af-2c-e6-c1-87 L2CAP PSM 0x13 (interrupt) open: connected to "Nintendo RVL-WBC-01"
IOBluetooth: scan ended: connected Nintendo RVL-WBC-01 (34-af-2c-e6-c1-87)
Wiimote: found device 0 "Nintendo RVL-WBC-01" at 34-af-2c-e6-c1-87 (IOBluetooth) (Balance Board by its name)
IOBluetooth: 34-af-2c-e6-c1-87 report #1 (8 bytes): a1 20 ...        (the first 5 reports, raw)
... then the BalanceBoard(real) lines below
IOBluetooth: 34-af-2c-e6-c1-87 "Nintendo RVL-WBC-01" disconnected: the device closed L2CAP PSM 0x13
```

Failures name the step and the IOReturn code (`baseband connection failed: IOReturn 0x...`,
`L2CAP PSM 0x11 failed to open: ...`, `connection timed out`). With the board off, the baseband
step fails (seen: 0x0000000a).

### Test tool (outside Cemu)

`build/tools/wiimote_bt_test` (built with Cemu from the same `.mm`; its own embedded Info.plist):

- `build/tools/wiimote_bt_test scan [seconds]`: scans (default 60 s, ends at the first device
  connected), then sets LED 1, requests the status, initialises the extension, reads the
  extension ID (0xA400FA; a board answers `00 00 a4 20 04 02`), sets report mode 0x34 and prints
  every report for 5 s, then disconnects.
- `build/tools/wiimote_bt_test connect 34-af-2c-e6-c1-87`: the same without the inquiry (press the
  sync button first).

Checked here without the board in sync mode: the scan runs and ends on time, other devices are
logged, connect-by-address fails cleanly at the baseband step.

## Pairing on macOS (hidapi path, needs working pairing)

Not verified on this Mac; these are the usual steps for Wii Remotes with hidapi-based emulators
(Dolphin on macOS also uses hidapi and relies on the system pairing):

1. Put batteries in the board (4x AA, the cover underneath). Open **System Settings > Bluetooth**.
2. Press the **red sync button** in the battery compartment. The blue power light blinks.
3. The board appears as **Nintendo RVL-WBC-01** (it may show only an address first). Click
   **Connect**. No PIN is normally needed; if macOS asks for one, cancel and try again (press
   sync again), or try leaving it empty. Do this while the light blinks (about 20 s).
4. macOS may list it as connected and then drop it after a few seconds if no app opens it: have
   Cemu already running (Input settings open is fine) so the provider opens it within 2 s.
5. After a power off, press the red sync button again (the front button only reconnects to a
   paired Wii). If the board never reappears, "Forget This Device" in Bluetooth settings and pair
   again.

Cemu's own Wiimote scanning needs no button: once macOS has it connected, the provider picks it up.
On this Mac pairing fails (see above); use **Connect real Wii device** instead.

## Setting it up in Cemu

1. Enable **Debug > Logging > Input API**.
2. **Options > Input settings**, the tab with the **Balance Board** emulated controller (or make
   one, see "Using it" above). Remove the keyboard from it (the keyboard would still work as
   Power, but isn't needed), pick API **Wiimote**, refresh, choose the board's entry
   (`Controller N`) and add it. Power gets mapped to the board's A (front) button.
3. The bottom line of the panel now says **Source: real Balance Board (Wiimote N): ...** with the
   state (not connected / not a Balance Board / reading its calibration / ready, CRC ok) and a
   second line with the live kg per sensor (relative to the factory 0 kg points), total,
   temperature and battery (byte and level 0-4). The option sliders are greyed out. With no
   Wiimote controller assigned the line says **Source: emulated.** as before.

## What the log should show

```
Wiimote: found device 0 "Nintendo RVL-WBC-01" at <path> (Balance Board by its name)
BalanceBoard(real): device 0 status: battery 0x.., flags 0x.. (bit 0x02 = extension connected)
Wiimote: device 0 extension ID 00 00 a4 20 04 02
BalanceBoard(real): device 0 is a Balance Board, reading its calibration
BalanceBoard(real): device 0: report mode 0x34 (continuous)
BalanceBoard(real): device 0: calibration bytes 0x20: 01 xx 00 00 ...   (16 bytes)
BalanceBoard(real): device 0: calibration bytes 0x30: ...               (16 bytes)
BalanceBoard(real): device 0: calibration bytes 0x60: xx 01
BalanceBoard(real): device 0: calibration CRC xxxxxxxx stored / xxxxxxxx computed: ok, version 1, ...
BalanceBoard(real):   TR: raw ~ / ~ / ~ at 0 / 17 / 34 kg   (x4)
BalanceBoard(real): device 0 report #1 (19 bytes): raw TR .. BR .. TL .. BL .. = ... kg, temperature .., battery 0x..
... reports #2-#5, then every 6000th
BalanceBoard: source is the real board (Wiimote device 0): ready, the game gets its load cells
BalanceBoard: WBCSetupCalibration(): real board, CRC .../... ok, ...
BalanceBoard(real): device 0: sent WPADControlBLC command 0x00 to register 0xf1
```

## Checks for the first test

1. Pairing: the board shows up as connected in Bluetooth settings and the `found device` line
   appears; the blue light turns steady (LED 1).
2. The extension ID line reads `00 00 a4 20 04 02`, the three calibration lines arrive, the CRC
   says **ok**. Real calibration values are roughly increasing per sensor (0 < 17 < 34 kg points).
3. Live readout: empty board ~0 kg per sensor (a few hundred grams of drift is normal); stand on
   it: the total is near your weight; lean front/back/left/right and check that the matching
   sensors go up (TL/TR are the "top" pair; the panel's dot should move the way you lean). If
   left/right or front/back are swapped, note which.
4. Power button: "Power pressed" appears in the panel line.
5. Wii Fit U: the weigh-in should give your weight (the game's zero point step removes drift).
6. Without the Wiimote controller on the Balance Board tab, the emulated board works as before.
