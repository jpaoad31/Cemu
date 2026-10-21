# jpaoad31/Cemu fork notes (branch `wii-fit-u`)

This fork supports the Wii Fit U decomp (`~/github/jpaoad31/wii-fit-u`) on macOS
(Apple Silicon, Metal). Changes go here as the decomp needs them.

## Done

One commit per fix, so upstream candidates can be offered separately.
Committed (on `wii-fit-u`, newest last): the memory range and the Balance Board (08026ec), H264 (0ff9865),
the graphic-pack code overlay (ecbd4a0), `--dump-nus` (88006ec), the WUA conversion error (3cca1e1).

- **`SYSTEM_LIBS` memory range** (0x01000000-0x017FFFFF, readable). Wii Fit U reads past its
  two-entry pointing-device table (`jet::ui::UICore::Read`) into 0x011004xx. That range is mapped
  on hardware and unmapped in Cemu, so it crashed with a bus error just after boot.
- **Emulated Balance Board:** see `BALANCE_BOARD.md`. The `padscore` WBC/BLC functions, data
  format 12, the controller type, the settings panel, and the mouse fallback for the centre of
  balance.
- **H264, multi-slice pictures** (`H264Dec.cpp`): a decode unit now runs through every slice of a
  picture, not just the first. Wii Fit U's videos (all 80 in `content/movie/`) use 3 slices per
  picture; Cemu assumed one, so libavc got part-pictures: green, concealed middles (0x26e) and bogus
  new pictures (0x462). Upstream has the same code.
- **H264, decode errors** (`H264DecBackendAVC.cpp`, `H264DecInternal.h`): a failed frame completes
  without output and `Execute` returns `BAD_STREAM`, so the game's own resync runs. The game used to
  hang briefly and show an untouched buffer. The log line now gives libavc's error code.
- **Graphic-pack `code/` overlay** (`CafeSystem.cpp`, `GraphicPack2.*`): a pack's `code/Step.rpx` now
  really replaces the RPX. Stock Cemu read the executable before registering pack files, and had a
  leading slash in the redirect path. Upstream has the same bug.
- **`--dump-nus <folder> -o <out>`:** decrypts a NUS title with Cemu's own code and the title's ticket.
  It skips linked FST entries (an update's FST lists the base's unchanged files).
- **Title Manager:** a failed WUA conversion shows an error instead of closing silently. The cause of
  the v81 conversion failure is still unknown.
- **Known, minor:** Cemu can crash (signal 11) on exit after a game ran ("Game info: Not running").

## Backlog (later)

- **PRIORITY: Wii Fit Meter (the user's favourite games, the step and altitude ones, need it).**
  - How it connects: IrDA over the GamePad's IR port. The game imports `nsysccr` (`CCRCDCPerIrdaControl`)
    and has its own wrappers `__CCRCDCIRCConnect`/`Disconnect`/`Send`/`Receive`. Its meter code is "SAMU",
    fully named in the symbols: `SAMUProcess`, `SAMUMiiIcon`, `CreateSAMULCDTexture`,
    `ClearIsReceiveSAMUData`, `EndFitmeter`, the "SAMU" thread.
  - Cemu has no `nsysccr` IR support; `nn_ccr` is one placeholder function.
  - Plan:
    1. GamePad IrDA HLE (`nsysccr` IR calls) with a pluggable peer device.
    2. An emulated Fit Meter speaking the meter protocol, learned from the game's SAMU code (decomp
       work in `../jpaoad31/wii-fit-u`): steps, the altitude log, the Mii icon, settings, the clock.
    3. Test controls: set or generate steps and altitude, then sync.
  - Hardware (stretch): the user owns a Fit Meter and a Raspberry Pi Pico debug probe.
    - Sniff real meter <-> GamePad IR with a Pico and an IrDA transceiver, to check the protocol.
    - Or bridge the real meter to Cemu over USB.
    - Firmware RE/decomp: identify the MCU and its debug access (SWD?) first.
- **Camera on macOS.** `src/Cafe/OS/libs/camera/camera.cpp` is a stand-in: it returns the game's
  own buffers untouched at 60 Hz, so Wii Fit U's weigh-in photo comes out blank. Feed real
  AVFoundation frames, converted to the format the game's `CAMSubmitTargetSurface` buffers
  expect (check the game's CAM* calls in `step.dis`), with the macOS camera permission
  (`NSCameraUsageDescription` in the bundle).
- **An input expression editor** like Dolphin's: mouse axes, combined inputs, `timer()` and so on.
  Cemu only maps keys, buttons and sticks; the mouse is wired to GamePad touch and gyro.
- **Real Balance Board passthrough** in the native Wiimote provider: the extension ID is wrong
  (0x2A2C), and there's no 0x34 parsing or calibration read. The macOS HID path is untested.
- **Upstream:** the `SYSTEM_LIBS` range is a general fix (Cemu has a precedent in HIGHMEM for
  Project Zero). Check Cemu's contribution rules on AI-written code before offering it.
- Further quality-of-life items and bug fixes as the decomp turns them up.
