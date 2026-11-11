# Remote GamePad (fork addition, uncommitted until tested)

An iPhone app (`~/github/jpaoad31/wiiu-gamepad-remote`) acts as the Wii U GamePad's controls:
buttons and sticks from a Backbone/MFi controller (or on-screen controls), the phone's motion as the
GamePad gyro/accelerometer, and the phone's screen as the GamePad touchscreen. Phase 1 is controls
only; video (phase 2) and rumble/audio (phase 3) come later. Protocol: the app's `PROTOCOL.md`.

## Use

1. Options → Input settings → Controller 1 → Emulated controller: **Wii U GamePad**.
2. **+** → API **RemoteGamePad** → Port (default 26770) → Controller: **Remote GamePad (UDP 26770)** → **Add**.
   The default mapping is 1:1 (the phone sends GamePad buttons), and "Use motion" is on. Adding a
   Remote GamePad **takes over** every GamePad button, stick and Home, even ones another controller
   (e.g. the keyboard) already had: Cemu allows one controller per GamePad button. Other controllers
   still only fill unmapped buttons. The mapping comes from `RemoteGamePad::kGamePadButtons`, and the
   unit test checks each protocol bit through to its VPAD hold bit.
3. On the phone: Settings → pick this Mac (Bonjour) or type its IP → Start.
4. The input settings show the port, the connected phone, its packet rate and the longest gap
   between packets over the last 3 s.

Touch from the phone goes through `VPADController::update_touch` as real GamePad touch and takes
priority over the mouse while the finger is down. Motion goes through the controller motion path
(`has_motion`/`get_motion_sample`, Mahony fusion), the same one SDL and DSU controllers use.

**Quick taps**: a press (button or touch-down) that started since the last game read is reported
to that read as down even if already released (`RemoteGamePad::PressLatch`; touch keeps its
position). The latch is consumed at the end of VPADRead/WPADRead/KPADRead through
`ControllerBase::on_emulated_read_done()`, so it sits at the read boundary; releases keep their real
timing. With protocol 1.1 the phone's press counters also catch presses between packets or in a lost
packet. Unread latches expire after 250 ms. `CEMU_REMOTE_GAMEPAD_TRACE=1` logs each read that got an
already-released press (`Remote GamePad trace: game read got latched press(es)`).

## Video, camera, microphone (phase 2, macOS)

- **Sockets** (`RemoteGamePadUdp.{h,cpp}`, macOS/Linux): each port has an IPv4 socket and an
  IPv6-only socket (Bonjour on the phone may resolve to IPv6), and every reply leaves **from the
  address the request was sent to** (IP_PKTINFO / IPV6_PKTINFO). The phone's NWConnection drops
  replies from any other source; with Wi-Fi and Ethernet on one subnet a socket bound to "any"
  answered a request to the Wi-Fi address from the Ethernet one, and the phone heard nothing.
  `tools/send_test.py reach --cross` checks this on the Mac. (Windows keeps boost sockets.)
- **Media port**: the control port + 1 (26771), announced in HELLO_ACK. One media port per Cemu.
- **Video**: `LatteRenderTarget_itHLECopyColorBufferToScanBuffer` calls
  `Renderer::CaptureRemoteGamePadFrame` for every DRC scan-out while a phone is subscribed (whether
  or not the GamePad view window is open). Metal draws the DRC texture into an IOSurface-backed
  BGRA CVPixelBuffer (output copy shader, sRGB-aware; 854×480 by default, see "Video size" below),
  and the command buffer's completion
  handler hands it to VideoToolbox (H.264, low-latency rate control, 60 fps, keyframes on request,
  bitrate from the phone's feedback). Fragments go out from the encoder callback. Vulkan/OpenGL
  don't capture (no video).
- **Camera**: `CreateCameraBackend` (macOS) switches between the phone (CAMERA_FRAGMENTs decoded
  to NV12) and the Mac camera. The Mac camera only starts while it's the source.
- **Mic**: `mic_updateOnAXFrame` reads 96 samples per 3 ms frame from the phone's 32 kHz stream
  (40 ms jitter buffer) instead of Cemu's input device while the phone is the source.
- **GamePad speaker** (protocol 1.3): `AIInitDRCDMA` (`snd_core/ax_out.cpp`), where every 3 ms AX
  DRC frame (144 frames, 48 kHz s16 big-endian, 2 or 6 interleaved channels) is copied for the
  GamePad audio device, first calls `RemoteGamePad::Media::SubmitPadAudio`. While a phone with the
  audio capability is subscribed and "GamePad audio" isn't *Mac*, the hub takes front L/R, packs
  240 frames (5 ms) per PAD_AUDIO datagram (992 bytes, ~1.6 Mbit/s, sent from the AX thread to the
  address the phone's media packets came from) and, with *phone*, returns true so the Mac's GamePad
  device is fed silence (still fed, so the AX pacing that waits on the device is unchanged). The
  phone's raw mix is sent before Cemu's GamePad volume; the phone has its own slider. When no phone
  plays audio, the cost is one atomic load per 3 ms.
- **Input settings** open on the Remote GamePad (if the GamePad has one) rather than the first
  controller: it has taken over the GamePad's buttons, and showing the empty keyboard entry made
  it look unsaved (the profile did save it).
- **Settings**: Input settings, under the status line of a Remote GamePad controller: "GamePad
  camera: phone when connected, else Mac / Mac camera", "GamePad mic: phone when connected /
  Cemu's audio input device" and "GamePad audio: phone when connected, else Mac / Mac (GamePad
  audio device) / phone and Mac", saved in the controller profile (`<camera>`, `<mic>`, `<speaker>`).
- **Video size** (`<video_size>` in the profile): "GamePad video: 854×480 (GamePad native)" (default) /
  "source resolution (up to 1280×720)" / "up to 1920×1080 (1.5×)" / "up to 2560×1440 (2×)". *Source*
  is the DRC image's host size (`LatteRenderTarget_itHLECopyColorBufferToScanBuffer` reports it every
  GamePad scan-out through `Media::SetVideoSourceSize`; a resolution graphic pack scales it), fitted
  inside the cap with its aspect kept, never upscaled. Wii Fit U renders the GamePad image at
  1280×720, so "source" sends 1280×720, and the larger caps only matter with a resolution pack. When
  the wanted size changes (setting or source) and holds for 300 ms, the hub rebuilds the encoder and
  pixel-buffer pool at the new size (the renderer takes the size from each buffer); the phone gets
  new SPS/PPS with the next keyframe and rebuilds its decoder. Bitrate: the start rate and the
  default cap grow with the square root of the pixel count relative to 854×480 (6 / 16 Mbit/s there;
  720p 9 / 24, 1080p 13.4 / 36, 1440p 18 / 48, cap at most 50; a cap from the phone still wins). The
  phone app needs no change: its display layer is stretched to the 16:9 touch area whatever the
  stream size, and touch is sent normalised (0..1). The selftest checks 1280×720 and back.
- **Restarts**: the hub sends a keyframe unasked when GamePad frames resume after 0.5 s or more (game
  stopped and started, pause) and whenever the encoder is (re)built; a buffer rendered from an old pool
  at another size is skipped, not encoded into the new session. If VideoToolbox can't create an encoder
  at the wanted size, it's logged and 854×480 is sent instead. Frame ids start at 1 in a new Cemu
  process: the receivers (phone app, Cemu's FrameAssembler for the camera) treat a fragment more than
  120 frames older than the newest completed one as a restarted stream (reset, wait for a keyframe)
  instead of as late. The selftest covers a pause, a restart with ids from 1 plus a size change, and
  2560×1440.
- The status line also shows how evenly the game delivers GamePad frames (frames/s, mean interval
  ± spread, max, histogram <12/~17/~25/~33/>40 ms, frames skipped because the encoder was busy)
  and the encoder mode. The fallback (non-low-latency) encoder also makes keyframes only on request.
- The status line adds the video (fps sent, Mbit/s and target, fps on the phone, latency with its
  Mac/arrival/decode parts, drops), the camera and mic sources, and the GamePad speaker: where it
  plays, PAD_AUDIO packets/s against the target 200, Mbit/s, mixed→sent, and the phone's buffer
  level/target, latency estimate, underruns and late/dropped packets (from its feedback).

## Files

| File | |
|---|---|
| `src/input/api/RemoteGamePad/RemoteGamePadProtocol.{h,cpp}` | packet parsing/building, motion axis conversion (no Cemu deps) |
| `src/input/api/RemoteGamePad/RemoteGamePadProtocolTest.cpp` | standalone test (not in CMake) |
| `src/input/api/RemoteGamePad/RemoteGamePadTransport.h`, `...TransportUDP.cpp` | transport interface; UDP (dual-stack) + Bonjour (`_wiiugamepad._udp`) |
| `src/input/api/RemoteGamePad/RemoteGamePadUdp.{h,cpp}` | IPv4 + IPv6 sockets, replies from the request's local address (no PCH) |
| `src/input/api/RemoteGamePad/RemoteGamePadMedia.h` | media interface for the provider, renderer, camera and mic |
| `src/input/api/RemoteGamePad/RemoteGamePadMediaCore.{h,cpp}` | packetiser, frame assembler, bitrate controller, mic jitter buffer (no Cemu deps) |
| `src/input/api/RemoteGamePad/RemoteGamePadMediaApple.cpp`, `...MediaNull.cpp` | media port, VideoToolbox encoder, camera decoder, mic (macOS); stubs elsewhere |
| `src/input/api/RemoteGamePad/RemoteGamePadMediaTest.cpp`, `...TestShim.h` | standalone media test with a synthetic picture (not in CMake) |
| `Renderer.h`, `MetalRenderer.{h,cpp}`, `LatteRenderTarget.cpp` | DRC capture |
| `camera/CameraBackendMacOS.cpp`, `mic/mic.cpp` | phone camera / mic as sources |
| `snd_core/ax_out.cpp` | GamePad speaker tap (`AIInitDRCDMA`) |
| `src/input/api/RemoteGamePad/RemoteGamePadControllerProvider.{h,cpp}` | receive thread, handshake, input acks (latency), rate/gaps, motion |
| `src/input/api/RemoteGamePad/RemoteGamePadController.{h,cpp}` | the controller: buttons, sticks, touch (`has_position`), motion |
| `InputAPI.h`, `ControllerFactory.cpp`, `InputManager.{h,cpp}`, `input/CMakeLists.txt` | registration |
| `VPADController.cpp`, `WPADController.cpp`, `EmulatedController.{h,cpp}`, `api/Controller.h` | default mapping; read-done hook for the press latch |
| `InputAPIAddWindow.{h,cpp}`, `InputSettings2.{h,cpp}` | port field; status line; Settings dialog (Use motion) |

The provider binds its port only when a Remote GamePad controller is created (profile load or the
Add dialog), and retries every 2 s if the port is taken.

Test without the phone:

    clang++ -std=c++20 -Wall -I src src/input/api/RemoteGamePad/RemoteGamePadProtocolTest.cpp \
      src/input/api/RemoteGamePad/RemoteGamePadProtocol.cpp src/input/api/RemoteGamePad/RemoteGamePadMediaCore.cpp \
      -o /tmp/rgtest && /tmp/rgtest
    ~/github/jpaoad31/wiiu-gamepad-remote/tools/send_test.py send --host 127.0.0.1 --port 26770
    # media (synthetic picture, no game needed); build line at the top of the file:
    /tmp/rgmedia selftest
    # with a game drawing the GamePad screen:
    ~/github/jpaoad31/wiiu-gamepad-remote/tools/send_test.py video --host 127.0.0.1 --out /tmp/v.h264
    # GamePad speaker (a game running, "GamePad audio" on phone), or against "/tmp/rgmedia serve 26790" with --port 26790 --tone:
    ~/github/jpaoad31/wiiu-gamepad-remote/tools/send_test.py audio --host 127.0.0.1 --out /tmp/pad.wav

## Motion axes

The phone sends the GamePad frame (+X right, +Y top edge of the screen, +Z out of the screen; gyro
rad/s; accel in g with CoreMotion's sign, face up = z −1). `ToMotionHandlerFrame` turns that into
exactly what the SDL path feeds `WiiUMotionHandler` for an SDL pad whose face is the GamePad's face:
gyro `(gx, −gz, gy)`, accel `(ax, −az, ay)`. The test checks this against the SDL path's formulas.
Not yet checked against a real GamePad's numbers in a game.
