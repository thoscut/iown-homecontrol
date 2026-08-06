# Velux io-homecontrol: on-air frame analysis & key-recovery status

Findings from a hands-on RX session with an SX1262 (Heltec V4.2) sniffing a real
Velux installation (window opener + wall remotes, model **BG-RC011-02** = KLI 31x
family). The goal is to control the actuators from the ESPHome component, which
requires the installation's AES-128 system (stack) key.

> **RESOLVED.** The "non-standard extended format" below was wrong, and the
> captures themselves disprove it. The frames are ordinary io-homecontrol frames
> wrapped in the UART start/stop bit framing this repo's own `docs/radio.md`
> already documents (start bit `0`, eight data bits **least-significant first**,
> stop bit `1` — ten bits per byte on air). A plain FSK radio hands those bits up
> as bytes without removing them, so a 25-byte frame arrives as ~48 bytes whose
> size field and CRC never match. De-framed, **all 173 captured frames validate a
> CRC-16/KERMIT** and decode to standard frames — including a **1W key transfer
> (0x30)**, which §6 said did not occur. The de-framing is now in the component
> (`iohome_phy_framing.{h,cpp}`, wired into receive and transmit) and host-tested
> against these very captures (`test/test_phy_framing/`). See **§0** for the
> resolution; the sections after it are the original investigation, kept as a
> record and marked where superseded.

**TL;DR (corrected)**
- Radio, CRC and crypto in this repo are **proven correct** against the reference
  vectors in `docs/linklayer.md`, and now against 173 real frames.
- The captured frames are **standard io-homecontrol frames** under a UART framing
  the radio does not strip in hardware. De-framed, every one has a valid
  CRC-16/KERMIT. There is no OEM "third protocol" here.
- A **1W key transfer (0x30)** *is* present in the capture (manufacturer byte
  `0x01` = Velux). It carries the masked key + manufacturer + sequence, and **no
  MAC** — so the CRC is its only integrity check.
- The system key is **still not confirmed** from passive captures: the documented
  address-mask de-masking reproduces the synthetic `linklayer.md` vector but does
  **not** reproduce this installation's command MACs (checked by brute-forcing all
  2²⁴ mask addresses), so either the OEM masking differs or the transferred key is
  not the one that signs commands. Key recovery remains open; hardware extraction
  (SWD) is still the fallback, not the only hope.

> **Reconciled with `docs/VELUX-FORMAT.md`.** That document (from the KLF 200
> specification) describes what a command *means* — Main Parameter direction
> (up=0x0000, down=0xC800, stop=0xD200), originators, priority levels, the 16-bit
> node type. This document describes the bytes on air. They now agree: de-framed,
> the "single byte `0x97` at idx14" was a framing artifact, and the frames carry
> the standard 2-byte Main Parameter the spec describes.

---

## 0. Resolution — the frames are standard, under UART framing

The io-homecontrol PHY sends each byte the way a UART does: a start bit (`0`),
the eight data bits **least-significant first**, then a stop bit (`1`). This is
in `docs/radio.md` already ("8-bit bytes with 1 start bit and a stop bit … the
least significant bit is transmitted first"). Ten bits on the wire per logical
byte.

An SX1276 or SX1262 in plain FSK mode has no hardware that removes this framing.
After the sync word it hands the raw bitstream up as bytes, start and stop bits
included. Read directly, a 25-byte frame becomes ~31–48 bytes whose control-byte
size field is wrong and whose CRC never validates — which is exactly what §3
found and misread as a "non-standard 48-byte format".

De-framing is one pass: read the bitstream most-significant-bit-first, and for
each ten-bit cell take the eight data bits (LSB first) as one output byte,
checking the start bit is 0 and the stop bit is 1. Applied to the raw captures in
`captures/`:

- **173 / 173** frames validate CRC-16/KERMIT.
- Frame lengths come out 14–31 bytes, all within the 34-byte maximum.
- Command IDs are all known: 0x00 Execute, 0x03/0x04 private, 0x2E, **0x30 Send
  1W Key**, 0x39 Remove 1W controller, 0x3D challenge response.
- Two decode byte-for-byte to examples in `docs/commands.md`: a `0x03` body of
  `03 00 00`, and a `0x04` answer beginning `05` (the documented "OK" code).
- A `0x00` Execute reads `01 61 c8 00 00 00` — originator USER, ACEI valid, Main
  Parameter 0xC800 (close). Standard, and consistent with `VELUX-FORMAT.md`.

Why the earlier brute force missed it: it searched a *uniform* bit offset across
the whole stream. UART framing is not a uniform offset — it is a repeating
ten-bit cell with two bits discarded per byte. Different search space.

This is implemented in `src/protocol/iohome_phy_framing.{h,cpp}` and wired into
the component's receive (`receive_frame_` de-frames before parsing) and transmit
(`send_frame` frames before sending). The codec is host-tested against these
captures in `test/test_phy_framing/test_phy_framing.cpp`.

### The 1W key transfer, and why the key is still not confirmed

One captured frame is a `0x30` Send 1W Key, de-framed to 31 bytes:

```
fc 00 | 00013f | 2ca919 | 30 | d978a0f11b858334df2c5f357b83782d | 01 | 01 | 049a | 398d
ctrl  | dest   | src    | cmd| encrypted key (16)               |mfr |?  | seq  | crc
```

`mfr = 0x01` is Velux. There is **no MAC** — the frame is CRC-protected only, so
nothing in it lets the recovered key verify itself. (The earlier key-capture code
assumed a 6-byte MAC and a 35-byte frame; that was corrected to this layout.)

De-masking with the documented method — AES(TRANSFER_KEY, IV) keyed on the node
address — is what reproduces the synthetic vector in `docs/linklayer.md`. It does
**not** reproduce this installation's traffic: unmasking the key and using it to
recompute the MAC of a command from the *same* remote (`2ca919`, which sends both
the key transfer and ordinary `0x00` commands) does not match, and a brute force
over all 2²⁴ possible mask addresses finds none that does. So either Velux masks
the 0x30 differently from the documented 1W scheme, or the transferred key is not
the key that signs commands. The component now prints the de-masked value as an
explicitly **unverified candidate**, not a key to trust.

---

## 1. Radio setup — confirmed correct

Config in `esphome/components/iown_homecontrol/iown_homecontrol.cpp` matches the
io spec (`docs/RADIO-SETUP.md`): 868.95 MHz, 2-FSK, 38.4 kbps, 19.2 kHz deviation
(modulation index h = 1.0), NRZ, no shaping, sync word `57 FD 99`, 512-bit
preamble, radio CRC off, fixed-length RX (raised to 64 bytes so whole frames are
captured — `IOHC_RX_CAPTURE_SIZE`).

Demodulation is confirmed good, not merely deterministic:
- The programmed sync word `57 FD 99` appears on air exactly (a capture caught the
  next frame's preamble+sync tail `… 55 55 57 FD`).
- Long clean `55/AA` preamble runs and byte-identical repeats of the same frame.
- At −2 dBm a deviation/bitrate error would not flip 2-FSK bit decisions, and a
  bitrate error would wreck the preamble. Neither happens.

## 2. CRC and crypto — proven correct (calibration)

Reference vectors from `docs/linklayer.md` reproduce exactly (see scratchpad
`calib.ps1` from the session, reproduced here for the record):

- **CRC** — doc frame `F6 00 00003F 385762 000143D2000000 0599 123456789ABC 5FB0`:
  our CRC-16/KERMIT over all-but-last-2 bytes = `0xB05F`, matching the on-wire
  `5F B0` (LSB first). ✔
- **1W key crypto** — doc frame
  `FC 00 00003F ABCDEF 30 7E60491F976ADF653DB0ED785E49A201 02 01 1234 19E81EC43D5E 9BF2`
  (node `ABCDEF`, key `0102…16`, seq `1234`): `decrypt_1w_key` recovers
  `01020304050607080910111213141516` and `create_1w_hmac` = `19e81ec43d5e`. ✔

Because these pass, a captured frame that fails every CRC test means the frame is
**not in the standard format**, not that the bytes are wrong.

## 3. Captured frames do NOT match the standard format

> **Superseded by §0.** The conclusion in this section is wrong. The frames are
> standard; they were being read *with* their UART start/stop framing still on.
> The exhaustive search below tested a uniform whole-stream bit offset, which is
> not what UART framing is — a repeating ten-bit cell with two bits dropped per
> byte — so it could not find the fit. De-framed, all 173 validate KERMIT.

Exhaustively tested and all negative on real captures (18-algorithm CRC-16
catalogue + the io native `compute_checksum`, every start offset, every trailer
position, both bit orders, per-byte bit reversal, whole-bitstream bit offset
0–15 with both read/regroup endianness, 7-bit LFSR whitening with all 128 seeds,
KERMIT at every length up to the full 58 bytes). Nothing validates.

Conclusion ~~(wrong — see §0)~~: these are the "third protocol for OEMs" hinted
at in `docs/linklayer.md:40`. Velux frames run **36–58 bytes** — longer than the
5-bit size field (max 34) can describe — so the length encoding and trailer are
non-standard. Integrity is almost certainly the AES-MAC only (no plaintext CRC).

## 4. Velux extended command frame — field map

From 11 known-plaintext `37 C0` window-command frames (button known per frame:
UP / STOP / DOWN), column classification (scratchpad `format_map.ps1`):

```
idx  0  1 | 2 | 3 4 | 5 6 7 | 8 9 10 11 12 13 | 14  | 15 16 17 18 19 | 20..27      | 28..47
     37 C0 | T | 04 01| 7E 51 D2| F4 8F 00 50 14 34 | CMD | 00 40 10 05 41 | rolling(enc)| MAC(~20)
     ctrl  |tog| ---- dest ---- -- src --
```

- idx0-1 `37 C0` = ctrl0/ctrl1 (const). Standard dest@2-4 / src@5-7 offsets hold.
- idx2 = retransmit toggle: base `0x10`, `0x90` on the 2nd of each press's two TXs
  (it is dest[0]'s high bit).
- idx2-4 dest = `10 04 01` (the actuator); idx5-7 src = `7E 51 D2` (the remote).
- idx8-9 is **device-bound**: the same remote sends `F4 8F`, another remote
  (`7E 46 94`) sends `AD 31`, a third (`7E 5A 11`) sends `35 29` — a per-source
  field (device type / address extension).
- idx10-13 `00 50 14 34`, idx15-19 `00 40 10 05 41`: constant Velux param blocks.
- **idx14 = command**: UP=`0x01`, STOP=`0x97`, DOWN=`0x27`. (NOT idx8, where the
  standard layout and the current parser look; idx8 here is const `0xF4`.)
- idx20-27: constant within one press (both retransmissions) but changes each
  press; 8 consecutive UP presses gave idx20-21 = `6C 1C 5C 3C 7C 02 42 22` in
  press order — high-entropy, non-monotonic → an **encrypted rolling code**, not a
  plaintext counter.
- idx28-47: varies on every transmission; between a toggle pair idx28 flips ~1 bit
  (echoes the toggle) and idx29+ changes completely → **MAC / authenticator**.

So the header (idx0-19) is fully readable; the tail (idx20-47) is cryptographic
and needs the system key.

### Sample captured frames (plain hex, one per row; preamble trimmed)

```
# 37 C0 window commands, remote 7E 51 D2, actuator 10 04 01 (UP/STOP/DOWN at idx14)
37 C0 90 04 01 7E 51 D2 F4 8F 00 50 14 34 01 00 40 10 05 41 78 4D 93 8D 9D 44 D9 B2 D5 2F 1A CE 87 5E ED 5A 89 60 3D 6F 56 C2 75 A2 B5 5D D7 A2
37 C0 10 04 01 7E 51 D2 F4 8F 00 50 14 34 97 00 40 10 05 41 14 54 94 34 25 35 D2 F7 7D EB 2D 45 11 1B B3 C8 A8 6A 08 1D AC 34 DE 13 AD 32 5C 42
37 C0 90 04 01 7E 51 D2 F4 8F 00 50 14 34 27 00 40 10 05 41 34 4D 34 74 47 34 43 D6 2D 57 1A 49 3A 69 6F 19 03 0A 86 2F A9 92 11 8C FD D2 6E A0
# NOT 2W pairing - this de-frames to a 1W 0x30 key transfer (ctrl0 0xFC, 1W bit
# set). The raw 0x1F is the first framed byte, not a control byte. See §0/§5.
1F C0 10 05 01 7E 46 94 AD 31 06 53 70 F4 0B 47 DB 15 0D 83 16 5F 71 A5 F5 56 5B D6 0C 3D 5A 50 14 04 41 2C D3 95 8C FB A8 80 DF 0C 6E 8F 95 97 C5 77 DB C6 33 CC 13 4E 84 15
# 1W frames, other types
47 C0 10 04 01 7E 46 94 AD 31 4E 40 11 05 33 20 D5 92 54 9B 5D 4A F2 9D 61 82 0B 1F D7 4F C3 A3 63 70 A8 32 DF E5 C3 7E FB 62 55 B7 90
69 40 16 4D 3D 5B 4F D7 3C EF 60 58 10 04 01 25 C8 F5 50 16 6D 6B 4D 64 96 60 39 65 5E A2 F7 69 E3 F5 5A BF
```

## 5. Key recovery from captures — still open, for different reasons than first thought

> **Partly superseded by §0.** A 1W key transfer (0x30) *does* appear in the
> capture — the search here missed it because it ran on the framed bytes, where
> no `0x30` command byte is visible. The rest of this section's conclusion (the
> key is not confirmable from the capture) still holds, but not for the reason
> given: the 0x30 frame carries **no MAC**, so it is not self-verifying, and the
> documented de-masking does not reproduce this installation's traffic.

- **1W key transfer (0x30)** is present (§0): manufacturer `0x01` = Velux, a
  16-byte masked key, a sequence, and **no MAC** — CRC only. The claim here that
  it has a "self-verifying 6-byte MAC over `[0x30]+ciphertext`" is wrong; that is
  the 1W *command* MAC, not part of the key-transfer frame. De-masking with
  `AES(node-address-repeated, TRANSFER_KEY)` reproduces the synthetic
  `linklayer.md` vector but not this installation's command MACs (brute-forced
  over all 2²⁴ mask addresses, none match), so the OEM masking likely differs or
  the transferred key is not the command key.
- **2W key transfer (0x32)** — **there is none in the capture.** The `1F C0`
  frame this was based on is not a 2W key transfer at all: de-framed it is the
  same 1W `0x30` above (its control byte is `0xFC`, the 1W bit set, command
  `0x30`). `0x1F` was a framing artifact of its first byte. A full scan of the
  de-framed captures finds no `0x31`, `0x32`, `0x38` or `0x3C` — no 2W key
  transfer and no challenge request was recorded. So there is nothing here to
  crack a 2W key from, which is a stronger statement than "no anchor": the frame
  was never captured.

The system key is therefore still not *confirmed* from sniffing. Hardware
extraction (§6) remains the reliable route; the 0x30 de-masked candidate is worth
checking against later authenticated traffic before trusting it.

### The 2W traffic that *was* captured

De-framed, the 2W frames are a controller `7E E7 EE` (a box/gateway) talking to
three actuators — `D5 E0 35`, `84 43 77`, `93 79 6D`:

| Command | Direction | Body |
| ------- | --------- | ---- |
| `0x3D` challenge response | box → actuator | 6-byte nonce + CRC |
| `0x03` private command    | box → actuator | `03 00 00` + CRC |
| `0x04` private answer      | actuator → box | `05 …` (0x05 = OK) + status + CRC |

These are ordinary 2W session frames, and they decode cleanly now — the point
being that the "2W" side is not a different format either. What is *missing* from
the capture is the session set-up: no `0x3C`/`0x31` challenge request precedes the
`0x3D` responses in these recordings, so the exchange cannot be replayed or its
MACs checked offline without the system key. A capture that includes the
challenge request would let the 2W handshake be followed end to end.

> **For the component:** the 2W handshake it speaks (`0x3C` out, `0x3D` in;
> commands signed against the session nonce) now goes over the air correctly for
> the first time, because `send_frame()` applies the UART framing and
> `receive_frame_()` strips it — the same fix as for 1W. Before, the component's
> 2W frames were transmitted un-framed and no real device could have decoded them.
> The 0x3D challenge-response frame is one of the codec's round-trip test vectors.

## 6. Hardware key-extraction plan (the way forward)

The AES stack key is stored in every 2W device in the installation.

### Target A — BG-RC011-02 remote (recommended first)
- MCU: **Silicon Labs EFR32FG1P133F256GM48** (Cortex-M4, 256 K flash). See
  `docs/devices/velux/KLI31x/README.md`.
- SWD header (10-pin, 1.27 mm) documented there:
  `GND=pin2 · SWCLK=pin3 · SWDIO=pin5 · RESET=pin6 · 3V3=pin10 · SWO=pin1`.
- Tooling: J-Link + Simplicity Commander (native), or ST-Link/CMSIS-DAP + OpenOCD.
- Steps: `commander device info` / `commander security status` → if unlocked,
  `commander readmem --range 0x0:0x40000 --outfile dump.bin` (+ user-data page).
- A full flash dump yields the key **and** the firmware, from which the exact
  Velux frame/MAC construction can be reversed — solving §4/§5 at once.

### Target B — KLF200 gateway (fallback / holds the shared key)
- Two processors (photos in `docs/devices/velux/KLF200/`):
  **STM32F427** (Cortex-M4, main app/API/Ethernet) and **EFM32GG990F1024**
  (Cortex-M3, io radio stack — most likely key holder).
- No external SPI flash; firmware is internal → SWD dump of one/both MCUs via the
  test-pad field on the back. STM32 RDP / EFM32 debug-lock status unknown.
- The KLF200's official local API controls actuators but does **not** expose the
  key.

### The gating risk
Debug lock / readout protection. Security devices often ship locked; if so, SWD
readback is blocked and unlocking mass-erases the key. Then only fault-injection
(glitching) remains, which is out of scope for now. A quick `security status`
check tells us immediately per device.

## 7. Open questions for a follow-up session

The "central contradiction" that used to head this list — standard format vs. a
48-byte extension — is **resolved** (§0): the format is standard, the 48 bytes
were the UART framing. That answers, in passing, why
`github.com/rspaargaren/iohomecontrol` sees standard 32-byte frames with the
identical crypto (same transfer key, CRC-16/KERMIT, 1W HMAC and `encrypt_1w_key`,
same radio) while these captures did not: its maintained CC1101 path de-frames
the ten-bit cells (`decodeFrame`, the `*8 + *2` length maths) and this component
did not. The window-opener and roller-shutter remotes agree because the whole
installation is simply standard io-homecontrol.

What is genuinely still open:

- **The system key.** Not recoverable from these captures. The one `0x30` 1W key
  transfer carries no MAC to confirm a de-mask, and the documented address-mask
  does not reproduce the installation's command MACs (§5). No `0x32` 2W transfer
  was captured at all. Hardware extraction (§6) remains the route, or a fresh
  capture of a pairing that includes the key transfer *to a device whose address
  is known*, so the de-mask can be checked.
- **The 2W handshake, end to end.** The capture has `0x3D` challenge responses and
  `0x03`/`0x04` private exchanges but no preceding `0x3C`/`0x31` challenge request
  (§5). A capture that includes the request would let the session set-up be
  followed and its MACs checked once a key is in hand.
- **FP1 (tilt) direction.** `VELUX-FORMAT.md` settles the Main Parameter direction
  from the KLF 200 spec but not the sign of Functional Parameter 1 on a tilting
  blind. Still needs a capture of a known tilt command.
- **Is either key-holding device's debug interface locked?** (§6; needs a probe.)
- **A 2W key-receive counterpart.** The library has 2W key-transfer *builders* but
  no receiver that stores a delivered key. `handle_1w_key_transfer_` is the 1W
  equivalent; a 2W one (from a `0x32`) would complete the pairing-as-follower path.
