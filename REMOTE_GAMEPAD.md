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

## Files

| File | |
|---|---|
| `src/input/api/RemoteGamePad/RemoteGamePadProtocol.{h,cpp}` | packet parsing/building, motion axis conversion (no Cemu deps) |
| `src/input/api/RemoteGamePad/RemoteGamePadProtocolTest.cpp` | standalone test (not in CMake) |
| `src/input/api/RemoteGamePad/RemoteGamePadTransport.h`, `...TransportUDP.cpp` | transport interface; UDP + Bonjour (`_wiiugamepad._udp`) |
| `src/input/api/RemoteGamePad/RemoteGamePadControllerProvider.{h,cpp}` | receive thread, handshake, input acks (latency), rate/gaps, motion |
| `src/input/api/RemoteGamePad/RemoteGamePadController.{h,cpp}` | the controller: buttons, sticks, touch (`has_position`), motion |
| `InputAPI.h`, `ControllerFactory.cpp`, `InputManager.{h,cpp}`, `input/CMakeLists.txt` | registration |
| `VPADController.cpp`, `WPADController.cpp`, `EmulatedController.{h,cpp}`, `api/Controller.h` | default mapping; read-done hook for the press latch |
| `InputAPIAddWindow.{h,cpp}`, `InputSettings2.{h,cpp}` | port field; status line; Settings dialog (Use motion) |

The provider binds its port only when a Remote GamePad controller is created (profile load or the
Add dialog), and retries every 2 s if the port is taken.

Test without the phone:

    clang++ -std=c++20 -Wall -I src src/input/api/RemoteGamePad/RemoteGamePadProtocolTest.cpp \
      src/input/api/RemoteGamePad/RemoteGamePadProtocol.cpp -o /tmp/rgtest && /tmp/rgtest
    ~/github/jpaoad31/wiiu-gamepad-remote/tools/send_test.py send --host 127.0.0.1 --port 26770

## Motion axes

The phone sends the GamePad frame (+X right, +Y top edge of the screen, +Z out of the screen; gyro
rad/s; accel in g with CoreMotion's sign, face up = z −1). `ToMotionHandlerFrame` turns that into
exactly what the SDL path feeds `WiiUMotionHandler` for an SDL pad whose face is the GamePad's face:
gyro `(gx, −gz, gy)`, accel `(ax, −az, ay)`. The test checks this against the SDL path's formulas.
Not yet checked against a real GamePad's numbers in a game.
