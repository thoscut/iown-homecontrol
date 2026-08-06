# Velux io-homecontrol: on-air frame analysis & key-recovery status

Findings from a hands-on RX session with an SX1262 (Heltec V4.2) sniffing a real
Velux installation (window opener + wall remotes, model **BG-RC011-02** = KLI 31x
family). The goal is to control the actuators from the ESPHome component, which
requires the installation's AES-128 system (stack) key.

**TL;DR**
- Radio, CRC and crypto in this repo are **proven correct** against the reference
  vectors in `docs/linklayer.md` — the tools are not the problem.
- The captured Velux frames are **correctly demodulated** but use a **non-standard,
  extended frame format** (36–58 bytes, past the 34-byte max the 5-bit size field
  can encode) that does **not** carry a standard CRC-16/KERMIT trailer.
- The command frame header is fully mapped (addresses, command byte, device id);
  the tail (rolling code + MAC) is cryptographically derived and **opaque without
  the system key**.
- The system key is **not recoverable from passive captures** for these devices
  (no 1W key transfer occurs; the 2W transfer has no verifiable offline anchor).
- The realistic path is **hardware key extraction** (SWD) from a remote
  (EFR32FG1) or the KLF200 (STM32F427 + EFM32GG990).

> **Related — and an unresolved contradiction.** `docs/VELUX-FORMAT.md` (added on
> the expert-review branch, derived from the KLF 200 specification) documents the
> actuator *semantics*: Main Parameter direction (up=0x0000, down=0xC800,
> stop=0xD200), command originators, priority levels, the 16-bit node-type field.
> That work and this one are complementary — it describes what a command *means*,
> this describes the bytes actually seen on air. **But they do not fully reconcile:**
> the whole repo model (that spec, `scripts/io-homecontrol.ksy`, the C++) assumes a
> **standard frame ≤34 bytes with a KERMIT CRC trailer**, and the captures here do
> not fit it (48 bytes, no valid CRC; the command at idx14 is a single byte `0x97`
> for stop, not the spec's 2-byte `0xD200`). See §3 and §7 — resolving this is the
> first thing a follow-up session should look at with fresh eyes.

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

Exhaustively tested and all negative on real captures (18-algorithm CRC-16
catalogue + the io native `compute_checksum`, every start offset, every trailer
position, both bit orders, per-byte bit reversal, whole-bitstream bit offset
0–15 with both read/regroup endianness, 7-bit LFSR whitening with all 128 seeds,
KERMIT at every length up to the full 58 bytes). Nothing validates.

Conclusion: these are the "third protocol for OEMs" hinted at in
`docs/linklayer.md:40`. Velux frames run **36–58 bytes** — longer than the 5-bit
size field (max 34) can describe — so the length encoding and trailer are
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
# 2W pairing (remote-to-remote copy), ctrl0 0x1F = 2W; ~24-byte constant block = masked key
1F C0 10 05 01 7E 46 94 AD 31 06 53 70 F4 0B 47 DB 15 0D 83 16 5F 71 A5 F5 56 5B D6 0C 3D 5A 50 14 04 41 2C D3 95 8C FB A8 80 DF 0C 6E 8F 95 97 C5 77 DB C6 33 CC 13 4E 84 15
# 1W frames, other types
47 C0 10 04 01 7E 46 94 AD 31 4E 40 11 05 33 20 D5 92 54 9B 5D 4A F2 9D 61 82 0B 1F D7 4F C3 A3 63 70 A8 32 DF E5 C3 7E FB 62 55 B7 90
69 40 16 4D 3D 5B 4F D7 3C EF 60 58 10 04 01 25 C8 F5 50 16 6D 6B 4D 64 96 60 39 65 5E A2 F7 69 E3 F5 5A BF
```

## 5. Key recovery from captures — exhausted

- **1W key transfer (0x30)** is the only format-independent crackable path (mask =
  `AES(node-address-repeated, TRANSFER_KEY)`, self-verifying 6-byte MAC over
  `[0x30]+ciphertext`). A validated brute force (scratchpad `keybrute.ps1`, with a
  synthetic positive control that recovers a known key) found **no 1W key** in any
  capture — including a deliberate actuator anlern/registration. These devices are
  effectively **2W-only**.
- **2W key transfer (0x32)** did occur (the `1F C0` pairing frames carry the
  masked key), but it is **not offline-crackable here**: the mask IV and the 2W MAC
  depend on frame-specific byte ranges of the Velux-extended format that we cannot
  identify, so there is **no verifiable anchor** — a recovered key can't be checked.

The system key therefore cannot be obtained by sniffing. It must be read from a
device that holds it.

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
- **Standard vs. extended format — the central contradiction.** The repo model
  (`scripts/io-homecontrol.ksy`, `docs/VELUX-FORMAT.md`, the C++) assumes a
  standard frame ≤34 bytes with a KERMIT CRC. These captures are 48 bytes and
  validate no CRC under exhaustive testing (§2, §3), yet the demod is provably
  good (the doc reference frames validate; the sync word is confirmed on air).
  Either these specific BG-RC011-02 remotes use a proprietary extension, or there
  is a demod subtlety not yet seen. **Confirmed across both a window-opener remote
  and a roller-shutter remote** (source `7E 5A 11`, session E) — the whole
  installation uses this 48-byte format, so it is not one odd device. Full raw
  captures are in [`captures/`](captures/) so this can be re-examined with fresh
  eyes. Resolving it is the prerequisite for everything downstream.
- **Related work — a working project with the *same crypto* but the *standard*
  format.** `github.com/rspaargaren/iohomecontrol` (ESP32, raw SX1276) controls
  Velux devices and shares this repo's crypto exactly: transfer key
  `34c3466e…4373`, the same CRC-16/KERMIT (`radioPacketComputeCrc`), the same 1W
  HMAC and `encrypt_1W_key`, radio at 38.4 kbps / 19.2 kHz dev. But its
  `MAX_FRAME_LEN` is 32 and its frames validate — i.e. **its devices use the
  standard format these captures do not.** Its method is the target end state:
  the ESP generates its own 1W key and pairs *itself* to the actuator with a `0x30`
  send, then drives it with `open`/`close`/`stop` while managing sequence numbers
  in NVS to avoid the desync PR #1 describes. Worth asking that author whether the
  48-byte format here is a Velux generation they recognise.
- Is either target's debug interface actually locked? (needs a debugger + probe)
- From a flash dump: locate the 16-byte key and reverse `construct_iv` / MAC for
  the Velux-extended format (idx20-47), then verify by reproducing a captured
  frame's MAC.
- Does `handle_1w_key_transfer_`-style logic need a 2W receive counterpart once the
  format is known? (see `docs/linklayer.md` 2W push/pull; repo has builders but no
  receiver that stores a key.)
