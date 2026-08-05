# Hardware bring-up

Everything this library claims about the io-homecontrol wire format is checked
against captures in `docs/` and reproduced by 197 host-run tests. None of it has
been checked against a physical actuator. That is the single largest open item
in `PRODUCTION_READINESS.md` (**K1**), and it is the one thing a laptop cannot
close.

This document is the order to do it in, what a pass looks like at each step, and
which open questions a session with real hardware can actually settle.

---

## 0. What you need

- A LoRa32 board with an SX127x or SX126x at 868 MHz. Supported out of the box:

  | Board | Environment | Radio |
  |---|---|---|
  | Heltec WiFi LoRa 32 V2 / V2.1 | `heltec_wifi_lora_32_V2` | SX1276 |
  | Heltec WiFi LoRa 32 V1 | `heltec_wifi_lora_32` | SX1276 |
  | Heltec WiFi LoRa 32 V3 | `heltec_wifi_lora_32_V3` | SX1262 |
  | Heltec WiFi LoRa 32 V4 | `heltec_wifi_lora_32_V4` | SX1262, **pins unconfirmed** |
  | Heltec Wireless Stick / Lite | `heltec_wireless_stick[_lite]` | SX1276 |
  | TTGO LoRa32 v1 / v2 / v2.1.6 | `ttgo-lora32-v1` / `-v2` / `-v21` | SX1276 |
  | LilyGO T-Beam | `ttgo-t-beam` | SX1276 |

  V2.1 is a minor revision of the V2 and uses the same environment.

- An io-homecontrol actuator. A Velux window or blind, a Somfy motor - anything
  that talks the protocol.
- Ideally a **second** board, so you can watch one talk while the other listens.

> **Legal note.** 868 MHz is licence-exempt in the EU under duty-cycle limits.
> Listening is unrestricted. Transmitting into a neighbour's installation is
> not something to do by accident - keep the power low and know which node IDs
> are yours before you send anything.

---

## 1. Does the radio answer?

Flash the listener and open the serial monitor:

```sh
pio run -e heltec_wifi_lora_32_V2 -t upload -t monitor
```

The first thing it prints is its pin map:

```
iown-homecontrol starting
[PINS] board: Heltec WiFi LoRa 32 (V2)
[PINS] CS=18 RST=14 SCK=5 MISO=19 MOSI=27
[PINS] DIO0=26 DIO1=35 (SX127x)
```

**Pass:** it continues to `[PHY] RSSI (dBm): -110` or similar and then
`Listening for io-homecontrol frames`.

**Fail:** `[FATAL] radio.beginFSK failed: -2`. That is
`RADIOLIB_ERR_CHIP_NOT_FOUND` - the radio did not answer over SPI at all, which
is nearly always a pin map that does not match the board. The firmware says so
and reprints the pins. Fix them in `src/board_pins.h`, or pass them as build
flags:

```ini
build_flags = ${env.build_flags}
              -DIOHC_BOARD_CUSTOM
              -DIOHC_RADIO_SX127X -DIOHC_RADIO_CLASS=SX1276
              -DIOHC_PIN_CS=18 -DIOHC_PIN_RST=14
              -DIOHC_PIN_DIO0=26 -DIOHC_PIN_DIO1=35
              -DIOHC_PIN_SCK=5 -DIOHC_PIN_MISO=19 -DIOHC_PIN_MOSI=27
```

To find the right macro name for a board, `tools/list_board_macros.py`.

### The V4 in particular

`[env:heltec_wifi_lora_32_V4]` carries the **V3's** radio pins, because
PlatformIO has no V4 manifest and nobody has confirmed the numbers. Step 1 is
exactly the test: if it reaches "Listening", the pins are right and the entry
can be marked confirmed. If it prints `-2`, correct the `IOHC_PIN_*` flags in
`platformio.ini` and it is settled either way in two minutes.

---

## 2. Does it hear anything?

Press a button on the actuator's remote, or operate it from a wall switch, while
the listener runs.

Every packet the radio delivers is printed raw, before the protocol layer forms
any opinion about it:

```
[RAW] f80000007f70875800016 1d40080c800003bd50552687549 9c7e72 rssi=-64 snr=9.5 len=27
```

**Pass:** `[RAW]` lines appear when, and only when, the actuator is operated.
That alone confirms the frequency, the bit rate, the deviation and the sync
word - four settings that were all wrong in this codebase at some point, and
that produce total silence when they are.

**Nothing at all?** In order of likelihood:

1. Wrong channel. Frames go out on 868.25, 868.95 and 869.85 MHz; the listener
   defaults to 868.95. Try the others (`IOHC_NODE_ID` sits next to the channel
   in `src/main.cpp`).
2. Sync word. It has to be programmed bit-reversed - `57 FD 99`, not the
   `FF 33` that appears over the air. Deriving one from the other by shifting
   yields `00 FF 33`, which matches nothing. `configure_radio()` gets this
   right; a hand-rolled setup may not.
3. Antenna. An SX127x transmitting without one can damage itself; receiving
   without one just does not work well.

**Statistics** print every 30 seconds:

```
[RX] received=42 accepted=0 crc=0 mac=0 replay=0 plain=42 malformed=0
```

`received` counts everything the radio delivered. The rest say what happened to
it. `plain=42` here means 42 frames arrived without a MAC this controller could
check, which is exactly what you see when you are not paired with the actuator -
that is not a fault, it is the expected state before pairing.

---

## 3. Do the captures decode?

Save the serial log and feed the raw frames to the decoder:

```sh
grep '^\[RAW\]' capture.log | awk '{print $2}' > frames.txt
./scripts/Iown-IoHexFrameParser.py -f frames.txt
```

```
io-homecontrol frame: length=27 (size field 24 + 3), from_addr=708758, to_addr=00007f
  control byte 1=0xf8 (order=command group end, protocol mode = One-Way)
  control byte 2=0x00 (use beacon=False, routed=False, low power mode=False, ack=False, protocol version=0)
  command ID=0x00
  command data=0161d40080c80000
  sequence=3bd5
  MAC=05526875499c
  CRC=7e72 (correct)
```

**Pass:** `CRC=... (correct)` on frames from your own installation.

A correct CRC on a real capture is the strongest single confirmation available
without pairing: it means the frame boundary, the size-field bias (+3) and the
CRC polynomial and bit order are all right simultaneously. Getting any one of
them wrong makes it fail.

**This is also the data worth keeping.** Please attach captures to an issue -
they are what turns the open questions below from arguments into answers.

---

## 4. Open questions a capture can settle

Each of these is currently marked unverified in the code. None needs pairing;
all of them need is a capture of the right button being pressed.

### K2 - the Velux command IDs

`src/velux/iohome_velux.h` declares `VELUX_CMD_*` at 0x58-0x5D, all marked
`UNVERIFIED`. They appear in no document in this repository and sit in the range
the standard uses for naming and info commands.

**Experiment:** operate a Velux window through every function its remote offers
- open, close, stop, ventilation position, rain-sensor query - and look at which
command IDs appear. If nothing outside 0x00/0x01 shows up, these constants
describe commands that do not exist and should be deleted.

### The direction of Functional Parameter 1 (tilt)

`create_tilt_frame()` assumes FP1 counts closure, like the Main Parameter does:
0x00 open, 0xC8 closed. That is inferred from the shared scale, not observed.
The ESPHome cover got this backwards until recently.

**Experiment:** on a pleated blind (Velux FML) or any slatted product, tilt the
slats fully open from the remote and capture the frame. Read the byte at offset
5 of the payload. `0x00` confirms the assumption; `0xC8` means both
implementations need inverting.

### The Execute payload length

Captures show both 6 and 8 payload bytes for command 0x00 - the documented
minimum plus two more functional parameters. The parser handles both.

**Experiment:** capture a range of commands and note which lengths appear.
Anything other than 6 or 8 is new information.

### K9 - the 2W pairing exchange

`pair_device_2w()` sends the 0x32 key transfer without the 0x3c challenge
request that `docs/linklayer.md` shows. The crypto underneath is verified
against a captured exchange; the frame sequence is not implemented.

**Experiment:** capture a real pairing between a controller and an actuator.
The order and direction of 0x31/0x38, 0x3c, 0x32, 0x3d and 0x33 is what is
missing, and it is not something to guess - a wrong guess here writes a key
neither side can use.

---

## 5. Transmitting

Only once steps 1-3 pass, and only with node IDs you know are yours.

`src/main.cpp` is a listener. To send, set `IOHC_NODE_ID` to an address your
installation does not already use, set `IOHC_SYSTEM_KEY` to your installation's
key, and call `controller.close(node)` or `controller.set_position(node, 50)`.

Without the right system key the actuator will ignore you: the MAC will not
verify. That is the protocol working, not a bug. Obtaining the key means
capturing a pairing - see `docs/SECURITY-MODEL.md` for what that implies, and
for why the transfer key being a public constant makes it possible at all.

### Fixed packet length

RadioLib's FSK mode sends exactly the programmed number of bytes, but
io-homecontrol frames vary from 11 to 34. `src/main.cpp` programs the maximum,
which works for receiving; for transmitting, install the length hook so each
frame is sent at its own length:

```cpp
controller.set_packet_length_callback(
  [](uint8_t len, void* ctx) -> int16_t {
    return static_cast<SX1276*>(ctx)->fixedPacketLengthMode(len);
  },
  &radio);
```

See `docs/RADIO-SETUP.md`.

---

## 6. Reporting back

What is useful, in descending order:

1. **Raw captures.** `[RAW]` lines with a note of what was pressed.
2. **Which step failed and what it printed.** The pin map and the statistics
   line together narrow almost everything down.
3. **Board and actuator model.**

A capture that contradicts something in `docs/` is the most valuable thing
here. Every fix in this library so far came from one document disagreeing with
another.
