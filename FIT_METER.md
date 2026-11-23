# Virtual Wii Fit Meter (branch `fit-meter`)

Wii Fit U can register and sync a Fit Meter whose data comes from a file. Cemu plays the GamePad's IR
port. A virtual meter sits on the other end of the link.

## Using it

1. **Tools > Virtual Fit Meter**. The window has:
   - an on/off switch;
   - "press the button automatically";
   - a **Press meter button** button;
   - registration status: Mii name, meter ID, date registered;
   - the meter clock and the last sync;
   - **Steps today** (Set writes it into activity.json);
   - **Load activity.json...**, **Open folder** and **Reset meter (unregister)**.
2. In Wii Fit U, register the meter as you would a real one (the Fit Meter settings).
   - The game asks you to point the meter at the GamePad and press its button.
   - With auto press on, the meter connects about 1.5 s after the game starts waiting.
   - Otherwise click **Press meter button**: it connects if the game is waiting now or within 10 s.
3. Syncing works the same way: the game's Fit Meter transfer (or the one at launch) connects, reads, and saves.
4. Logging: **Debug > Logging > CafeOS modules > Fit Meter / IR**. It is off by default. It logs every
   frame both ways, decoded down to the SAMU command, plus the meter's decisions.

The meter folder is `<Cemu user data>/fitmeter/` (`~/Library/Application Support/Cemu/fitmeter/` on macOS).

| file | what |
|---|---|
| `meter.json` | Settings (`enabled`, `auto_press`, `auto_press_delay_ms`, `activity_file`). Registration: `registered`, `save_data_id`, `samu_id`, `ident_flag`, `mii_name`, `mii_info` (hex), `personal` (hex). Clock: `clock_set`, `clock_offset_s`. Bookkeeping: `registered_at`, `last_sync`, `sync_count`, and the list of blobs. |
| `blobs/<cmd>_<arg>.bin` | What registration wrote. `82_xx` are the LCD texts, images and sounds; `83_00..07` are the Mii icons; `86..8B` are debug writes. |
| `activity.json` | The data the meter "measured" (below). |

Registration writes to the **game's save** too. The profile stores the meter's ID, the pairing date and
the transfer date. Back up `mlc01/usr/save/00050000/10102300` (Wii Fit U USA) before trying it.

### activity.json

```json
{ "default_tag": 0,
  "days": [
    { "date": "2026-11-22", "steps": 6543, "kcal": 210,
      "segments": [ { "start": "07:30", "minutes": 25, "mets": 3.5, "tag": 2, "climb_m": 15 } ],
      "mets": [ ... ], "altitude": [ ... ], "tag": [ ... ] }
  ] }
```

- `steps` and `kcal` are per day.
  - When kcal is missing it is estimated as steps × 0.04.
  - Days between the oldest day and today that have no entry are sent as 0.
  - Values are capped at 65529.
- Per minute data is optional.
  - It can be `segments`: start, length, METs, tag, and metres climbed (spread over the minutes).
  - Or it can be raw arrays of up to 1440 values from 00:00: `mets` (as METs, e.g. 3.5), `altitude`
    (absolute metres), `tag` (0..29).
  - Without per-minute data the meter sends "no data" for banks 02-04. The game then keeps those minutes empty.
- `tools/fitmeter/sample_activity.json` is an example.

## How it works

```
game IRC lib --CCRCDCPerIrdaControl / VPADBASEGetIRCStatus--> DrcIrLink (the GamePad's IR engine)
                                                                  |  IRC payloads
                                                                  v
                                                     IrPeer: VirtualFitMeter (IRCU slave + SAMU + store)
                                                                  |
                                                     MeterDataSource: JsonFileMeterSource (activity.json)
                                                                  |  (phase b: the iPhone Remote GamePad app)
```

| file (`src/Cafe/OS/libs/nsysccr/`) | what |
|---|---|
| `nsysccr.cpp` | The HLE modules: `nsysccr.CCRCDCPerIrdaControl`, plus `vpadbase.VPADBASEIsInit` (returns 1, so IRCInit initialises), `VPADBASEGetIRCStatus` and `VPADBASEClearIRCEvent`. Also `FitMeterHost`, the thread-safe API the UI uses. |
| `DrcIrLink.*` | The GamePad's IR engine (below). |
| `VirtualFitMeter.*` | The meter. |
| `FitMeterActivity.*` | The dates, the JSON source, `SetDayStepsInFile`, and the encoder. |
| `FitMeterProtocol.*` | CRC-8, frame build/parse, XOR, the log descriptions. |
| `FitMeterTest.cpp` | The standalone tests (see Verification). |

`DrcIrLink` handles the four requests:

| request | what it does |
|---|---|
| CONNECT | Arms the link with its timeout. When the peer "presses the button", status bit 0x2 rises. If the timeout passes first, RECEIVE returns result 1, which IRCU turns into its timeout error 23 and the game retries. |
| SEND | Parses the frame, fills in the session id and the CRC-8, and hands the payload to the peer. |
| RECEIVE | Pops one frame for the console. Status bit 0x1 stays set while frames are queued. |
| DISCONNECT | Drops the link. |

The game retries a 1000 ms connect while it waits. Back-to-back attempts count as one "pointing", so the
auto press is not reset by each retry. A test found this; without it the meter would never link.

`VirtualFitMeter` is the IRCU slave plus the SAMU handler.
- **IRCU slave:**
  - `F4` command: a write gets `F2`; a read starts sending chunks.
  - Chunks are `F0` / `F1`, sized from the console's rxSize hint (at most 509).
  - Each `F2` from the console gets the next chunk. The console's `F3` (its ack of `F1`) gets `F3` back.
  - `F5` gets the last packet resent.
  - On a link the meter started (connect type 0), it first sends `F3` ("ready").
- **SAMU:** XOR 0xAA both ways.
  - Reads: ident, time, banks 02-06.
  - Writes: 0x80-0x85 and 0x86-0x8B, `0xF0` (initialize: unregister), `0xF2` ("not your meter": logged).
- **Clock:** the meter's time is host time plus `clock_offset_s`. The game's time write (0x81) sets the
  offset; the meter keeps the host's seconds.

## Protocol notes (Wii Fit U v81)

Sources: the asm of IRC (`IRCProc`, `__CCRCDCIRC*`), `main/ircu.cpp`, `jet::SAMU` (`ThreadMain`,
`Send`, `Receive*`, `ProcessBulk*`), the decompiled `jet::SAMUSeq`, and `SAMUProcess`. See also
`~/temp/wii-fit-u/fitmeter/FIT_METER_RESEARCH.md`.

### IRC requests and frames

- **Requests** (big-endian): `00 timeout16 bitrate rxSize16 target type` (8 bytes), `01 size16 frame…`
  (520), `02` (reply 520: `result size16 frame`), `03`.
- **Frame:** `A5 sid len [len2] rxSize16 payload crc8`.
  - The game writes sid 0 and leaves a CRC slot; the GamePad fills both.
  - `rxSize` is the reply size + 6 (+7 above 61; 5 for no reply).
  - IRCProc returns error 5 on a control frame (bit 0x80) and never sees Create/Accept/Close.
- **Status bits:** IRCProc moves connecting to connected on bit 0x2, connected to idle when 0x2 drops,
  and calls RECEIVE on bit 0x1.
- **IRCUConnect:**
  - Mode 0 (what SAMU uses): CONNECT type 0 with rxSize 1, then wait up to 3 s for the meter's `F3`.
  - Mode 1: the console sends `F3` itself.

### IRCU transfers

The packet size is 509 (`.data 0x1014D764`).

- **Write:** `F4 cmd|80 arg` → `F2`, then each `F0` chunk → `F2`, then `F1` → `F3`.
- **Read:** `F4 cmd arg` → chunks.
  - The console acks `F0` with `F2` and `F1` with `F3`, then waits for the meter's `F3`.
  - The console needs exactly `size` bytes; more is error 20.

### SAMU commands

| cmd | arg | contents |
|---|---|---|
| read 0x00 | 0 | Ident, 9 bytes: `saveDataId u32, samuId u32, flag`. `IsRegisteredIdentData` is `flag == 0`. |
| read 0x01 | 0 | Time, 6 bytes: `flag (0 = set), year-2000, month 1-12, day, hour, minute`. |
| read 0x02-0x04 | bytes wanted (10080 from SAMUSeq) | The per-minute banks. |
| read 0x05-0x06 | 120 | Kcal and steps per day. |
| write 0x80 | 0 | Ident. `samuId` is the CRC32 of the profile id XOR the calendar time, made by the game. The last byte is `0xFF` for a person and `0x00` for a pet. |
| write 0x81 | 0 | Time, 6 bytes, flag 0. |
| write 0x82 / 0x83 | `<bank>` | From `82_xx.dat` / `83_xx.dat` file names, or the Mii icon number 0..7. |
| write 0x84 | 0 | `FFLStoreData` (96 bytes) + 6 zero bytes. The name is at 0x1A as UTF-16 (Wii U: big-endian). |
| write 0x85 | 0 | Personal data, 15 bytes. Byte 3 is the day change (night owl: the day starts at 3:00). |
| write 0xF0 / 0xF2 | 0 | `Send(240/242, 0, NULL, 0)` sends **one byte 00**. `0xF0` is sent only by the Initialize flows (SAMUSeq modes 1/2) when the meter is registered; the game then clears the profile's meter ID. `0xF2` means "not registered / not yours". |

### Sequences (SAMUSeq)

| flow | steps |
|---|---|
| Pairing (mode 3) | Ident (must be unregistered) → time, `samu_common_dat`, `samu_sound_dat`, the language archive, personal, 8 Mii icons, Mii info, **ident last** → disconnect. |
| Sync (mode 4) | Ident (registered, same IDs) → time. If the clock is more than 600 s off, the game writes the time first. Then banks 02, 03, 04, 05, 06 (a pet reads 02, 03, 05). |
| Initialize (modes 1/2) | Ident → `0xF0` or `0xF2`. |

### Log streams

Pinned by running the game's decoder. Each stream starts with the newest value.

- **Banks 05/06 (u16 BE per day):**
  - Index 0 is "today" by the meter's clock. With the night-owl setting and an hour before 3:00, today is the previous day.
  - `FFFF` is skipped (padding).
  - `FFFC`/`FFFD` are date markers (`GetDatePos`); not used.
  - Steps are capped at 65529.
- **Banks 02/03/04 (u8 per minute):**
  - Index 0 is the meter's current minute.
  - `n 00` is n minutes of 0. Each other byte is one minute.
  - `FF` (and `FE`) are skipped.
  - `FC`/`FD` are date markers; not used.
  - 02 is METs × 10 (the graph's range is 0..140 + 30).
  - 03 is the altitude change in metres per minute: `01..7F` is +1..+127, `80..F9` is -1..-122 (`80+k` = -(k+1)). The first minute becomes the day's base height.
  - 04 is the activity tag 0..29 (`sMETsColor` has 30 entries; the meanings are unknown).
- The game stores altitude and tag minutes **only in days the METs stream created**, so all three per-minute streams start at the same minute.
- **Re-syncs** replace each day's values (no double counting). Sending the whole history every time is right.

## Verified vs inferred

**Verified**

- Build: release, macOS bundle.
- `FitMeterTest.cpp`: 234 checks, 0 failures. It contains a C++ model of the game's IRC + IRCU master, written from the asm. It covers:
  - frames, the CRC, the XOR, the rxSize mapping;
  - connect timeout → result 1;
  - registration with chunked 1200-byte writes;
  - persistence across a restart;
  - the clock offset;
  - a full sync of banks 02-06;
  - `F5` resend, the manual button, the game's initialize;
  - the encoder (caps, night owl, run lengths, altitude spill, no token split by the size limit).
- `tools/fitmeter/verify_encoder.py` passes: 26,673 checks, 0 failures, at 14:30 and at 01:15. It works like this:
  - It dumps the streams through the whole link (`fitmeter_test --dump`).
  - `decoder_harness.py` runs Wii Fit U's own `SAMUProcess::SAMUDataSave` from `step.elf` in the decomp's PowerPC interpreter. The save accessors are replaced by a model of `RPStepSAMUData`.
  - It compares steps, kcal, METs, tag and altitude for every minute with activity.json.
- A scratch run, with a portable folder built from copies, booted Wii Fit U with the new modules loaded. `vpadbase` / `nsysccr` no longer show as unsupported calls.

**Inferred** (from the asm, not seen against a real GamePad or meter)

- The exact status-bit edge cases.
- The RECEIVE result codes (1 = timeout).
- The CRC-8 parameters (the game never checks them).
- That the meter reports flag 0 once registered, whatever byte 8 was written. The game writes 0xFF for a person but requires 0 on read.
- That `0xF0` unregisters.
- The METs scale (×10).
- The tag meanings.
- The kcal estimate.

**Not yet seen:** the in-game registration/sync screens in a scratch run. This session had no screen-capture or input permission to navigate the game's menus, so no IR frames were logged in a run. That is the user's test.

## Phase (b): the phone as the data source

`MeterDataSource` (`FitMeterActivity.h`) is the seam. A `PhoneMeterSource` would cache the Remote GamePad
app's "meter sync" upload as an `Activity`, and `FitMeterHost::SetDataSource()` would install it
(`nullptr` goes back to the file). The protocol sketch is in the research doc §4.2 (`0x40-0x42`).
