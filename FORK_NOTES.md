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
- ~~Known, minor: Cemu could crash (signal 11) on exit after a game ran~~: the Metal layer bug, fixed with Stop emulation.
- **Camera on macOS** (uncommitted until tested): the GamePad camera shows the Mac's camera.
  - `camera.cpp`: structures per WUT's camera.h. Stream info is height at +4, width at +8 (Cemu had
    them swapped). The decode-done event passes surface/handle/failed (Cemu passed the size as the
    handle). Frames arrive at the requested rate (fps 1 = 30, 0 = 15) and are written into the
    submitted surface as NV12, 640x480, pitch 768 (Wii Fit U: 0x87000-byte surfaces, UV at
    +0x5A000). Black when there's no frame yet, no permission or no backend.
  - `CameraBackend.h` + `CameraBackendMacOS.cpp` + `CameraCaptureMacOS.{h,mm}`: AVFoundation, the
    default video device, 640x480 preset (or medium), NV12 video range; center-crop to 4:3 and
    nearest-neighbour scale. `CameraBackendNull.cpp` for other platforms.
  - Permission: `NSCameraUsageDescription` in `MacOSXBundleInfo.plist.in`; asks on first CAMOpen,
    starts once granted, logs if denied.

- **Stop emulation** (uncommitted): File -> "Stop emulation" (Cmd+.) in release builds (was debug-only, "unfinished -
  crashes after repeated use"). Stop -> relaunch in one session now works; what was missing in the shutdown:
  - Metal layer ownership (`MetalLayer.mm`, `MetalLayerHandle`): the handle released a layer it didn't own (copyable
    class, temporary destructor) and never removed its view, so AppKit later drew a freed layer: the crash after
    stopping, and the crash on quitting after a game ran. Now retained, move-only, view removed on the main thread.
  - Sys area (`coreinit_MEM.cpp`): a bump allocator never rewound, so each launch leaked ~8 MB and the 4th ran out.
    Rewound to the post-startup mark at ShutdownTitle; cached pointers into it cleared (Latte CP index array,
    placeholder font, nn_uds workspace).
  - AArch64 recompiler (`PPCRecompiler.cpp`): the instance data was re-reserved per title, but the AArch64 enter stub
    is generated once with its address baked in, so later titles' code used the freed range. Now reserved once.
  - Metal pipeline cache: async compile threads and cache loader/writer threads were detached and outlived the
    renderer; now stopped and joined in `MetalRenderer::Shutdown` (after waiting for the GPU), pending requests
    dropped; uninitialised members fixed.
  - Shader compile queues (Metal and Vulkan): queued jobs and semaphore counts survived StopThreads, so the next
    title's threads compiled freed shaders. Queue/semaphore cleared on stop; shaders unqueue themselves on delete.
  - Latte (`LatteThread.cpp`): the decoded-index cache and in-flight texture readbacks held the old renderer's
    objects; released/finished before the renderer shuts down.
  - Graphic packs (`GraphicPack2*.cpp`, `rpl.cpp`): packs were never deactivated and their module list kept freed
    modules, so a relaunch didn't re-apply patches; now deactivated at title end, module unloads notified, code caves
    released.
  - Test hook: `--test-stop-cycle CYCLES,SECONDS[,quit]` (launch with -g, stop after SECONDS, relaunch, CYCLES
    times, then quit; `,quit` quits with the game running on the last cycle). For automated regression runs.

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
- **An input expression editor** like Dolphin's: mouse axes, combined inputs, `timer()` and so on.
  Cemu only maps keys, buttons and sticks; the mouse is wired to GamePad touch and gyro.
- **Real Balance Board passthrough** in the native Wiimote provider: the extension ID is wrong
  (0x2A2C), and there's no 0x34 parsing or calibration read. The macOS HID path is untested.
- **Upstream:** the `SYSTEM_LIBS` range is a general fix (Cemu has a precedent in HIGHMEM for
  Project Zero). Check Cemu's contribution rules on AI-written code before offering it.
- Further quality-of-life items and bug fixes as the decomp turns them up.
