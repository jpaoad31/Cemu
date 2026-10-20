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
