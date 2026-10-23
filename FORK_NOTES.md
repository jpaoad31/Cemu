# jpaoad31/Cemu fork notes (branch `wii-fit-u`)

This fork supports the Wii Fit U decomp (`~/github/jpaoad31/wii-fit-u`) on macOS
(Apple Silicon, Metal). Changes go here as the decomp needs them.

## Done

One commit per fix, so upstream candidates can be offered separately.
Committed (on `wii-fit-u`, newest last): the memory range and the Balance Board (08026ec), H264 (0ff9865),
the graphic-pack code overlay (ecbd4a0), `--dump-nus` (88006ec), the WUA conversion error (3cca1e1),
the macOS camera (40e4ce9), Stop emulation and the shutdown fixes (1428abd).

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

- **Stop emulation** (1428abd): File -> "Stop emulation" (Cmd+.) in release builds (was debug-only, "unfinished -
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
- **Texture cache: no reuse across linear/tiled layouts:** saved as `~/temp/cemu-patches/texcache-tilemode-match.patch`
  (applies on 47cfc00), **not applied**: a real bug, but not the Wii Fit U snapshot cause. Neither the view lookup cache
  (its own "todo - add tileMode param") nor CanTextureBeRepresentedAsView compares tile modes, so a linear texture can
  be served by an older tiled texture with the same address/size/pitch/format (256x256 RGBA8: pitch 256 both ways).
  The patch adds TM_IsLinear (LatteReg.h), an optional tileMode filter on lookup/lookupWithColorOrDepthType
  (LatteTextureView), passes it from the sampler binding (LatteTextureLegacy) and stops CreateMapping reusing across
  a linear/tiled change. Upstream has the same code.
- **CPU access to GPU-rendered tiled textures** (committed; `GX2_Misc.cpp` GX2Invalidate, `LatteAsyncCommands.{h,cpp}`
  ASYNC_CMD_CPU_READBACK, `LatteTexture.h` lastCpuReadbackDynamicUpdate). Cemu mirrors render targets back to RAM
  only when linear (and a few tiny tiled ones), so a game that edits a tiled render target on the CPU works on stale
  RAM. Wii Fit U renders Mii icons (RPSysMiiIcon::MakeTexture: InitializeRenderTarget type 1, RGBA8, default tile
  mode = tiled) and thresholds their alpha in place on the CPU (BinarizationAlpha: GX2Invalidate 0x72 =
  CPU|texture|color|depth, GetImageBuffer, threshold, GX2Invalidate again); Cemu then reloaded the texture from that
  stale RAM: real scene colours scrambled into stripes and blocks, with cut-out alpha. Now GX2Invalidate with CPU +
  COLOR_BUFFER (not inside a display list) flushes, waits for the GPU and reads back any GPU-written tiled 8_8_8_8 2D
  texture at that address, once per GPU write (so the second call, after the CPU edit, doesn't undo it). CPU+TEXTURE
  only (0x42, after a CPU upload) is untouched.
- Note: Debug > Dump > Textures works (LatteTextureLoader_UpdateTextureSliceData), but only writes textures
  Cemu loads from memory after it's enabled: turn it on before entering a screen, not on it.
- **Metal: compressed <-> uncompressed copies via a buffer:** saved as `~/temp/cemu-patches/metal-block-copy-via-buffer.patch`
  (applies on 83e8415), **not applied**. Metal's texture-to-texture blit needs matching pixel formats, so a copy between
  e.g. a UINT_R16_G16_B16_A16 render target and the BC1 texture aliasing it does nothing. The patch routes such copies
  (same bytes per block, native formats, 2D) through a buffer. A candidate for FFL's GPU-compressed Mii face textures
  (FFLiCompressorImpl<BC1/BC3> renders into a 0x11F colour buffer aliasing the BC texture), but in a boot test the path
  never ran, so it's unconfirmed.
- **GPU colour clears reset the texture's RAM change tracker** (uncommitted; `LatteRenderTarget.cpp`
  LatteRenderTarget_applyTextureColorClear). Binding a colour buffer for drawing calls LatteTexture_UpdateDataToLatest,
  which reloads the texture from RAM if RAM's hash changed. A GPU clear doesn't touch RAM or the hash, so a CPU write
  made before the clear (Wii Fit U's body test: the icon slot first held the photo, or an earlier icon's CPU alpha
  pass) was reloaded over the clear at the first draw, and the Mii landed on a stale background. On hardware the clear
  overwrites memory, so the earlier CPU write no longer counts. Found with the gated `[TextureReadback-CPU]`
  diagnostics (GX2_Misc.cpp skipped-in-display-list line, LatteAsyncCommands.cpp per-candidate lines; Texture
  readback logging only).
- **Always expand all graphic packs** (committed; `GraphicPacksWindow2.*`, `GeneralSettings2.*`, `wxCemuConfig.*`): General settings checkbox, off by default; every folder in the graphic packs tree is expanded whenever the list is built (open, filter, reload).
- **ErrEula dialog can be confirmed** (committed; `erreula.cpp`, `WindowSystem.h`, `wxWindowSystem.cpp`): Wii Fit U's ErrorViewer only checks the buttons when `ErrEulaGetSelectButtonNumError` > 0, which Cemu didn't export (unsupported stub returned 0), so clicks were ignored; added it (plus no-op SetControllerRemo/IsSelectCursorActive/DrawTV/DrawDRC), fixed the inverted IsDecideSelectLeft/RightButtonError, GamePad/Wii Remote A (left/only) and B (right) via the ControllerInfo given to ErrEulaCalc, Enter/Space; labels skip MSBT tags, map PUA E000-E005 to (A)/(B)/(X)/(Y)/(L)/(R), fall back to OK; each AppearError logs the raw button text.

## Upstream (Cemu's AI policy)

Cemu's CONTRIBUTING.md (checked 2026-10-22): code from new contributors must be written and understood by a
human; AI is fine for planning, design, review and questions about the codebase, not for writing the code (not
even paraphrased). Reasons given: low review capacity, AI fixes that "work on the surface" but aren't accurate
emulation, and LLMs possibly trained on leaked SDKs. So nothing here goes upstream as a PR. What fits: detailed
bug reports (welcome per CONTRIBUTING.md; our commit messages are written that way), or the user writing a fix
themselves from our notes. Best candidates for reports: H264 multi-slice, graphic-pack code overlay, CPU readback
of tiled render targets, the clear undone by a stale reload, the unmapped 0x01000000 range, the shutdown bugs
behind "Close game".

**Plan (user, 2026-10-22):** at the end of the project, file a batch of detailed issues upstream (one per general bug,
with cause, evidence and repro), and make this fork public as a reference with all the fixes, stated clearly as
AI-written so maintainers can treat it as background, not as a PR. Keep commits one-per-fix with full messages, so
each issue can point at its commit. Check the licence terms (Cemu is MPL-2.0) and the original authors' notices when
publishing.

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
- **Unified memory for Metal (Apple Silicon):** the CPU and GPU share memory, but Cemu keeps guest RAM and GPU
  resources apart (built for discrete GPUs) and copies between them. Wrap guest RAM as a no-copy MTLBuffer and do
  readbacks/uploads as GPU compute passes that convert between Latte tiling and Metal textures directly in guest RAM
  (linear textures in place). Readbacks would get cheap enough to do whenever the game may look, which removes the
  stale-RAM class of bugs (like the Mii icon alpha pass) instead of fixing each case. Big: memory mapping, the Metal
  backend, the texture cache. After the Fit Meter.
- **An input expression editor** like Dolphin's: mouse axes, combined inputs, `timer()` and so on.
  Cemu only maps keys, buttons and sticks; the mouse is wired to GamePad touch and gyro.
- **Real Balance Board passthrough** in the native Wiimote provider: the extension ID is wrong
  (0x2A2C), and there's no 0x34 parsing or calibration read. The macOS HID path is untested.
- **Upstream:** the `SYSTEM_LIBS` range is a general fix (Cemu has a precedent in HIGHMEM for
  Project Zero). Check Cemu's contribution rules on AI-written code before offering it.
- Further quality-of-life items and bug fixes as the decomp turns them up.
