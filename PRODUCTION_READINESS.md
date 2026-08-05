# Production Readiness Assessment

## Overview

This document tracks the production readiness status of the iown-homecontrol
project, covering the C++ protocol library (`src/protocol/`), the high-level
controller (`src/IoHomeControl`), the Velux helpers (`src/velux/`) and the
ESPHome integration (`esphome/components/iown_homecontrol/`).

**Current Status: BETA**

The protocol layer is now verified against the byte-for-byte captures in
`docs/`, covered by 198 host-run unit tests, and hardened against the receive
path being attacker-controlled. What is *not* verified is behaviour against
real hardware: nobody has yet confirmed that a physical actuator obeys a frame
this library produces. Treat every "Complete" below as "complete and tested in
software".

---

## Issue Tracker

### ✅ FIXED

#### Protocol conformance

| ID | Severity | Component | Description |
|----|----------|-----------|-------------|
| P1 | **Critical** | `iohome_frame.h` | Control Byte 0 bit 5 is `isOneWay` (1 = 1W). The library treated it as "is 2W", so every transmitted frame announced the opposite protocol mode. |
| P2 | **Critical** | `iohome_frame.h` | `Size` is the frame length excluding Control Byte 0 and the CRC (total = Size + 3). The library used total = Size + 11, so the announced length was wrong by 8 bytes on every frame. |
| P3 | **Critical** | `iohome_constants.h` | `CMD_SET_POSITION`/`STOP`/`OPEN`/`CLOSE` (0x60-0x63) do not exist. Actuators are driven by command 0x00 with a Main Parameter. Cover control could never have worked. |
| P4 | **Critical** | ESPHome component | ACEI byte was 0x00. Its bit 0 is "IsValid" and actuators discard frames where it is clear. |
| P5 | **Critical** | ESPHome component | FSK data rate was passed as 38400/19200 where RadioLib expects kbps/kHz, so `setDataRate` failed and the component marked itself failed at setup. |
| P6 | High | `iohome_constants.h` | `FRAME_MAX_SIZE` of 32 was below the 34 bytes the 5-bit size field can describe. Both pairing frames failed to serialize, so pairing could never have worked. |
| P7 | High | `iohome_frame.cpp` | Authenticated and plain frames were not distinguished; every frame was assumed to carry a sequence number and MAC, which no discovery or key-transfer frame does. |
| P8 | High | Several | Command IDs 0x29/0x2A/0x2B/0x31/0x51/0x52/0x53 were mapped to the wrong meanings; corrected against `docs/commands.md`. |
| P9 | Medium | Both | Preamble length was divided by 8 before being handed to RadioLib, which takes FSK preamble length in bits: 64 bits instead of 512. |
| P10 | Medium | `IoHomeControl.cpp` | Sync word was derived from the OTA constant 0xFF33 by shifting, producing `{0x00, 0xFF, 0x33}` instead of the bit-reversed `{0x57, 0xFD, 0x99}` the radio needs. |
| P11 | Medium | Both | RadioLib's FSK defaults prepend a length byte and append their own CRC; neither is part of an io-homecontrol frame. Now disabled explicitly. |

#### Security

| ID | Severity | Component | Description |
|----|----------|-----------|-------------|
| S1 | **Critical** | `iohome_2w.cpp` | 2W challenges came from an unseeded Arduino `random()`. Every device produced the same challenge sequence after every reboot, so an attacker could precompute a valid response. Now backed by the platform CSPRNG, which refuses rather than falling back. |
| S2 | **Critical** | Receive path | No replay protection. A valid MAC proves authorship, not freshness, so any recorded frame could be replayed off the air. Added `ReplayGuard`. |
| S3 | High | `IoHomeControl.cpp` | Received 2W frames were validated with a null challenge, dereferencing it inside the IV construction - a remote crash from a single received frame. |
| S4 | High | `iohome_2w.cpp` | A challenge stayed live after a failed verification, letting an attacker grind responses against one known nonce. Challenges are now single-use. |
| S5 | Medium | `iohome_2w.cpp` | An authenticated 2W session never expired. Now bounded (30 s default). |
| S6 | Medium | `IoHomeControl.cpp` | Plain (unauthenticated) frames were accepted unconditionally. Now rejected unless they carry one of the commands the protocol defines as unauthenticated. |
| S7 | Low | `iohome_crypto.cpp` | Key material was left on the stack. Added `secure_zero()` on every path that touches a key, IV or MAC. |
| S8 | Low | `iohome_crypto.cpp` | `verify_crc16()` accepted a 2-byte buffer as a valid frame covering no data. |

#### Robustness

| ID | Severity | Component | Description |
|----|----------|-----------|-------------|
| R1 | **Critical** | `src/main*.cpp` | `main.cpp` and `main_IoHome.cpp` both defined `setup()`, `loop()`, `radio` and `phy`: the firmware could not link. The legacy sketch moved to `examples/`. |
| R2 | High | `IoHomeControl.cpp` | Reception used `scanChannel()`, which is LoRa-only and returns `ERR_WRONG_MODEM` in FSK, so no frame was ever received. Now interrupt driven. |
| R3 | High | `IoHomeControl.cpp` | The rolling code was written to NVS on every single command, wearing out the flash. Now uses block reservation. |
| R4 | High | ESPHome component | The rolling code was never persisted, so it restarted at 0 after every reboot and receivers rejected everything until it caught up. |
| R5 | Medium | `iohome_2w.cpp` | Discovery left the `DISCOVERING` state after the first answer, so only one device was ever found; it also accepted any frame as a discovery answer and never honoured its own timeout. |
| R6 | Medium | `iohome_2w.cpp` | Discovery and key-transfer frames were transmitted without being finalized, so they carried a zero CRC. |
| R7 | Medium | `iohome_2w.cpp` | Frequency hopping was timed with millisecond resolution, which cannot express the 2.7 ms dwell time. Now microseconds. |
| R8 | Medium | `IoHome.cpp` | `setPhyProperties()` had an unbounded output-power loop that spins forever on a module that rejects every level. |
| R9 | Medium | `IoHome.h` | `ntoh`/`hton` template bodies lived in the `.cpp`, so every external use failed to link. Moved to the header. |
| R10 | Medium | `IoHome.h` | Control Byte 0 macros put the mode bit at 7 and the order field at 5; both are wrong. |
| R11 | Medium | ESPHome component | Received main parameter was read from bytes 9-10 (originator and ACEI) instead of 11-12. |
| R12 | Medium | ESPHome cover | `publish_state()` on every loop iteration flooded the API while moving. Now throttled. |
| R13 | Medium | ESPHome `cover.py` | Used `cover.COVER_SCHEMA`, removed from current ESPHome; the platform failed to load. |
| R14 | Low | Everywhere | Missing nullptr guards in the frame setters, `is_broadcast()`, `print_frame()`, beacon and discovery handling, `get_rssi()`/`get_snr()`. |
| R15 | Low | `.github/workflows` | Path filters used bare directory names, which never match, so the PlatformIO workflow effectively never ran. |
| R16 | Low | `iohome_2w.cpp` | `BeaconHandler` recorded the claimed data length rather than the length it actually copied. |

#### Found while reviewing the fixes

| ID | Severity | Component | Description |
|----|----------|-----------|-------------|
| V1 | **Critical** | ESPHome component | The packet ISR was an inline `IRAM_ATTR` function storing to a `std::atomic`. On ESP32 that needs a literal-pool load, which the linker rejects: "dangerous relocation: l32r: literal placed after use". The firmware would not link. |
| V2 | **Critical** | ESPHome `__init__.py` | RadioLib's `Module.h` includes `<SPI.h>`, which ESPHome does not put on the include path unless the library is declared. The component did not compile. |
| V3 | High | `iohome_frame.cpp` | Deciding whether a frame carries an authentication trailer from the protocol mode alone is wrong: 2W frames carry a MAC too. Authenticated 2W frames were parsed with the MAC folded into the payload and then rejected. Now decided by command ID and payload length. |
| V4 | High | `iohome_2w.cpp` | Wiping the challenge on a successful handshake left the commands it authorises unable to be MAC'd, and inbound 2W frames verified against a zeroed nonce. |
| V5 | High | `IoHomeControl.cpp` | A peer-initiated 2W handshake could never be answered: command 0x3C carries the peer's nonce in its own payload, but verification insisted on a nonce of ours. |
| V6 | High | `include/iown_mac.h` | `iown_mac.h` and `iown_frame.h` included each other; `#pragma once` resolved the cycle by expanding the MAC header first, so the size macros it used were undefined. |
| V7 | Medium | `include/iown_mac.h` | `IOWN_IS_BROADCAST_ADDR` referenced a symbol that never existed and used `memcmp` without `<string.h>`, so any use failed to compile. The address was also sized with `IOWN_LEN_HEADER_MAC` - two node IDs - so the comparison read past the caller's buffer. |
| V8 | Medium | `library.properties` | Written as `key = "value"`. The Arduino spec requires `key=value` with no spaces or quotes, so the Library Manager would reject the library. `includes` also named a header that does not exist. |
| V9 | Medium | `library.json` | Not valid JSON - it carried a commented-out block written with `//`, so PlatformIO could not parse the manifest. |
| V10 | Medium | `src/esp32_api_spi.cpp` | `fInitializeSPI_Channel()` ignored its SPI host argument and always initialised `HSPI_HOST`. |
| V11 | Medium | `scripts/LuaJIT/` | `luajit-convertNested.py` did not parse: unescaped quotes in a `print`, and it opened a bare identifier instead of a filename. |
| V12 | Low | `iohome_frame.cpp` | `print_frame()` accumulated `snprintf`'s return value into a `size_t` offset unchecked, which would have underflowed the remaining-space argument had the buffer filled. |
| V13 | Low | ESPHome cover | Pressing stop mid-movement could send a second STOP if the timed estimate completed on that same call. |
| V14 | Low | `iohome_crypto.cpp` | The software AES fallback was selected on plain ESP-IDF builds, where mbedTLS is available; `esp_random()` moved out of `esp_system.h` in ESP-IDF 5. |
| V15 | Low | `.github/workflows` | CodeQL and the spell check were both disabled with `on: workflow_dispatch`. |
| V16 | Medium | Receive path | 2W frames had no replay protection at all: they carry no sequence number, and one session challenge signs several frames. Added a recent-MAC history per node. |
| V17 | Low | `iohome_replay_guard.cpp` | A node taking over an evicted table slot inherited the previous node's sequence number, so its first frames were rejected. |
| V18 | High | `iohome_2w.cpp` | `generate_challenge()` never stamped the challenge with the current time, so it was timestamped at zero. Every 2W handshake attempted more than the challenge timeout after boot expired instantly - invisible to tests that use timestamps near zero. |
| V19 | Low | `.clang-tidy` | The config carried `AnalyzeTemporaryDtors`, removed in clang-tidy 16, so the linter aborted with "unknown key" before analysing anything. `pio check` reported success without having checked a single file. |
| V20 | **Critical** | `iohome_crypto.cpp` | `encrypt_2w_key()`/`decrypt_2w_key()` built the key mask from a constant `0x55` padding plus the challenge, dropping the requesting frame's payload and checksum that `construct_iv_2w()` puts in bytes 0-9. A device paired this way received a key that decrypts to noise. Nothing caught it because both directions made the same mistake, so the round-trip test agreed with itself; it is now checked against the captured value in `docs/linklayer.md`. |
| V21 | **Critical** | `platformio.ini`, `src/main.cpp` | The firmware did not build. `RADIOLIB_ERR_INVALID_RADIO` does not exist in RadioLib - the native test mock had invented it - and the `LoRa32` dependency ships an example `main.cpp` in its `src/`, so the link step failed with "multiple definition of `setup'". Board pins now live in `src/board_pins.h`. |
| V22 | High | `iohome_frame.cpp` | Command 0x00's payload was treated as exactly 6 bytes. Captures show 6 and 8 - the minimum plus extra functional parameters - so the longer form missed the parser's precise length test, and a plain 8-byte Execute frame was read as authenticated with two parameter bytes taken for a MAC. |
| V23 | High | `include/iown_frame.h` | The length macros contradicted the protocol layer: a two-byte sync word (it is three), a 2W packet with no HMAC, a flat one-byte parameter, and no macro for the size-field bias. |
| V24 | Medium | `src/esp32_api_spi.cpp` | `fReadSPIdata16bits()` clocked three bytes out of a two-byte buffer and read the answer from the wrong offset; `fInitializeSPI_Devices()` hardcoded `HSPI_HOST`; `GetHighBits()` returned `int8_t` for a `uint8_t`. |
| V25 | Medium | `src/esp32_utils.cpp` | `iown_crc_calc()` had its whole body commented out and returned `-1` from a `uint16_t`, so every caller got 0xFFFF and could not tell it from a checksum. |
| V26 | Medium | `scripts/` | The Python toolchain could not start: `Iown-ioCrypto.py` did `import aes` and `Iown-IoHexFrameParser.py` did `from ioCrypto import ...`, but the files are `Iown-AES.py` and `Iown-ioCrypto.py` - hyphens cannot appear in a module name. |
| V27 | Medium | `Iown-IoHexFrameParser.py` | Source and destination addresses were swapped, the order field was read as two "first/last frame" flags, the size field was compared against the buffer length without the +3 bias, and the authentication trailer was never split off. |
| V28 | Low | `Iown-ioCrypto.py` | The 1W key-push demo appended a MAC to a frame whose own size field says there is none, making it 37 bytes - two more than the 5-bit field can express. Command 0x30 is a bootstrap command and travels plain. |
| V29 | Low | `include/` | `iown_node_types.h` used a C++ `enum class` inside an `extern "C"` block, so the headers could not be compiled as C despite advertising it. `iown_defs.h` used the reserved identifier `_IOWN_DEFS_H` and defined `IOWN_MODE_1W` after the header that uses it. |
| V30 | Low | `platformio.ini` | The firmware built with no warning flags at all, and RadioLib was pinned with a caret range that had drifted from 7.1.2 to 7.7.1. |
| V31 | Low | `src/board_pins.h` | The old board table keyed TTGO v2.1 on `ARDUINO_TTGO_LORA32_V21NEW` while PlatformIO defines `ARDUINO_TTGO_LoRa32_v21new`, so that board never compiled; and SX126x boards got BUSY passed as the interrupt line. |
| V32 | Medium | `iohome_constants.h` | `CMD_SERVICE_RESET` sat at 0xF1. Reboot is 0xF2; 0xF1 is "read groups" / service ACK, so the constant named one command and addressed another. Found by diffing our table against `docs/commands.md` and the enum in `scripts/io-homecontrol.ksy`, which agree with each other. The whole command table, the Main Parameter values, the originators and the ACEI layout are now pinned by tests. |
| V33 | **High** | `platformio.ini` | `check_flags` carried `--fix-errors`, so `pio check` rewrote the source tree in place. Combined with V34 it produced code that does not compile - `namespace iohome;` followed by `{`, and `static` on functions declared in headers - across 30 files. A command that checks must not edit. |
| V34 | High | `platformio.ini` | `.clang-tidy` was never applied. PlatformIO appends `--checks=*` unless `check_flags` mentions `--checks` or `--config`, and a command-line `--checks` overrides the file's list completely, so every documented exclusion was ignored and the run reported 1835 defects. Fixed with `--config-file=.clang-tidy`; the count is now ~20, all style. |
| V35 | Medium | `platformio.ini` | `check_skip_packages = yes` withheld the framework include paths, so clang-tidy could not find `stddef.h` or `Arduino.h` and aborted the parse of most files - the same shape as V19, a linter that reports success without having analysed anything. |
| V36 | Low | `src/IoHome.h` | Include guard `_IOHOME_H`: a leading underscore followed by a capital is reserved for the implementation. |
| V37 | High | ESPHome cover | Tilt was sent uninverted. FP1 shares the Main Parameter's scale, which counts closure, while ESPHome counts openness - so a request to open the slats closed them. The position path in the same file inverts in three places. |
| V38 | Medium | `src/velux/iohome_velux.h` | `create_tilt_frame()` took `tilt_percent` with no direction stated while its sibling takes an explicit `percent_open`; the implementation treated it as closure. That ambiguity is what V37 read the wrong way. It now takes `percent_open` and inverts internally. |
| V39 | **High** | `platformio.ini`, `.github/workflows/platformio.yml` | `pio run` builds only `default_envs`, so CI had always compiled exactly one board. Four entries in `src/board_pins.h` were keyed on macro names PlatformIO does not define - `ARDUINO_TTGO_LORA32_V21NEW`, `ARDUINO_TTGO_LORA32_V1`, `ARDUINO_TBEAM`, `ARDUINO_HELTEC_WIFI_LORA_32_V3` - and every one of those boards fell through to "unknown board" without a word. There is now an environment per board and a CI job that builds all of them. |
| V40 | Low | ESPHome component | The CRC and MAC initial value were duplicated from `src/protocol/` with nothing checking they agreed. They did - but so did the 2W key transfer, right up until it did not. Now extracted into a dependency-free header and compared byte for byte by `test_esphome_crypto`. |
| V41 | **High** | ESPHome component | The SX1262 path handed RadioLib the pins in the SX127x order, so an SX1262 only worked if you put its DIO1 in `dio0_pin` and its BUSY in `dio1_pin` - which is what the example config told people to do. Filling the fields in with what the pins are actually called produced a radio whose interrupt never fired. Same defect as V31, in the other implementation. The component now takes `busy_pin`, builds the Module per family, and the schema rejects every combination that cannot be wired. |
| V42 | Medium | ESPHome `__init__.py` | The component pinned RadioLib 7.1.2 while `platformio.ini` pinned 7.7.1, so the two halves of the repository compiled against different versions of the same library and only the firmware side was covered by `check_radiolib_mock.sh`. CI now compares the two pins. |
| V43 | Medium | `docs/devices/` | The tree carried both `Velux/` and `velux/` - 46 files each, 57 MB duplicated, and seven pairs that were not byte-identical. macOS and Windows use case-insensitive filesystems by default, so git cannot check both out: one overwrites the other and the working tree reads as permanently modified for the pairs that differ. The lower-case tree was kept and CI now rejects any two tracked paths that collide when lower-cased. |

### 🔶 KNOWN Issues (Not Yet Fixed)

| ID | Severity | Component | Description | Recommended Fix |
|----|----------|-----------|-------------|-----------------|
| K1 | High | Everything | No verification against real hardware. Every conformance claim rests on the captures in `docs/`. | Follow [`docs/HARDWARE-BRINGUP.md`](docs/HARDWARE-BRINGUP.md), which lists the experiments that settle K2, K9 and the FP1 tilt direction |
| K2 | Medium | `src/velux/` | `VELUX_CMD_*` (0x58-0x5D) are undocumented and unverified; they sit in the range the standard uses for naming/info commands. Marked UNVERIFIED in the header. | Confirm with a capture, or remove |
| K3 | Medium | ESPHome component | The 2W challenge-response handshake is not wired in, so 2W frames are sent unauthenticated. | Port `AuthenticationManager` into the component |
| K4 | Medium | ESPHome component | `position_feedback` accepts unauthenticated position reports. Off by default, marked experimental. | Verify the MAC before applying |
| K5 | Medium | `IoHomeControl` | Fixed-length FSK mode needs a per-transmission length change, which is not part of the `PhysicalLayer` interface. A hook is provided but the caller must install it. | Document per chip, or template on the radio type |
| K6 | Low | ESPHome component | Duplicates the CRC and MAC implementation from `src/protocol/` so the component stays self-contained for `external_components`. The duplicate now lives in a dependency-free header and `test_esphome_crypto` proves it agrees byte for byte, so this is a maintenance cost rather than a correctness risk. | Share the sources via a build-time copy |
| K7 | Low | Logging | Still printf-style rather than structured. | Consider structured logging |
| K8 | Low | `src/esp32_api*`, `src/iown_mac.cpp` | Older ESP32 helper layer, not covered by tests and not used by `IoHomeControl`. Its defects are fixed (V24, V25, V29) and the headers now compile as C in CI, but nothing exercises the SPI helpers at runtime. | Fold in or remove |
| K9 | **High** | `IoHomeControl::pair_device_2w` | Sends the 0x32 key transfer on its own. The documented exchange also has the controller send a 0x3c challenge request carrying the challenge, so the device knows which one to build its IV from. Pairing therefore works only against a device that already holds that challenge. | Send the 0x3c step, or drive pairing from a received 0x31 |

### 🔒 Security Considerations

See [`docs/SECURITY-MODEL.md`](docs/SECURITY-MODEL.md) for the full threat
model. Summary of what remains, by design of the protocol:

| ID | Severity | Description | Status |
|----|----------|-------------|--------|
| T1 | Info | Frames are authenticated, never encrypted. Addresses, commands and positions are visible to any listener. | Protocol limitation |
| T2 | **High** | The transfer key is a public constant, so a pairing frame reveals the system key to anyone listening. | Protocol limitation - pair close, pair rarely |
| T3 | Medium | Keys generated by Overkiz/TaHoma boxes come from a weakly seeded `math.random`, leaving on the order of 2^25 candidates. | Use `crypto::generate_system_key()` for new keys |
| T4 | Medium | The system key is stored in plaintext in RAM and NVS. | Enable ESP32 flash encryption |
| T5 | Info | The MAC is truncated to 48 bits. | Protocol limitation |
| T6 | Low | No rate limiting on pairing or discovery. | Operator-initiated, low risk |

---

## Component Status

### Protocol Library (`src/protocol/`)

| Feature | Status | Notes |
|---------|--------|-------|
| CRC-16/KERMIT | ✅ Complete | Reproduces the CRC of the captured frame in `docs/linklayer.md` exactly |
| AES-128 | ✅ Complete | mbedTLS on ESP32, bundled software AES elsewhere; both checked against FIPS-197 |
| MAC generation (1W/2W) | ✅ Complete | Constant-time verification |
| Frame construction | ✅ Complete | Length encoding verified against three captures |
| Frame parsing | ✅ Complete | Bounds-checked, swept under ASan over every control-byte combination |
| Authentication trailer | ✅ Complete | Plain vs. authenticated frames modelled explicitly |
| IV construction | ✅ Complete | 1W and 2W |
| Key masking | ✅ Complete | Both directions, with round-trip tests |
| Replay protection | ✅ Complete | `ReplayGuard`, wraparound-safe, LRU-bounded |
| CSPRNG | ✅ Complete | Fails closed when no secure source exists |

### High-Level Controller (`src/IoHomeControl`)

| Feature | Status | Notes |
|---------|--------|-------|
| 1W commands | ✅ Complete | Command 0x00 with Main Parameter |
| 2W commands | ✅ Complete | Requires an outstanding challenge |
| Receive path | ✅ Complete | Interrupt driven, CRC → MAC → replay, with statistics |
| Frequency hopping | ✅ Complete | Microsecond timing |
| Device discovery | ✅ Complete | Collects multiple devices, honours its timeout |
| Device pairing | 🔶 Untested | Frames are correct and validated; not tried against hardware |
| Beacon handling | ✅ Complete | |
| Rolling code persistence | ✅ Complete | Block-reserved NVS writes |
| Memory management | ✅ Complete | Destructor, `nothrow`, non-copyable |
| Input validation | ✅ Complete | |

### ESPHome Component

| Feature | Status | Notes |
|---------|--------|-------|
| Radio initialization | ✅ Complete | SX1276/SX1262, CRC and length byte disabled |
| Frame reception | ✅ Complete | ISR driven, CRC verified, length-field checked |
| Cover control | ✅ Complete | Open/Close/Stop/Position |
| Tilt support | ✅ Complete | Via Functional Parameter 1, opt-in |
| 1W authentication | ✅ Complete | With persisted rolling code |
| 2W authentication | ❌ Missing | See K3 |
| Position feedback | 🔶 Experimental | Unauthenticated, off by default |
| Diagnostic sensors | ✅ Complete | RSSI, frame counters, rolling code |
| Configuration validation | ✅ Complete | Key, ACEI, frequency, address and SPI-pin checks |

### Legacy Code (`src/IoHome.cpp`)

| Feature | Status | Notes |
|---------|--------|-------|
| `begin()` | ✅ Complete | Stores configuration and keys |
| `crc16()` | ✅ Complete | Delegates to `crypto::compute_crc16` |
| `setPhyProperties()` | ✅ Complete | Bounded power loop, correct preamble |
| `ntoh()`/`hton()` | ✅ Complete | Now header-defined so they link |

---

## Testing Status

| Area | Status | Notes |
|------|--------|-------|
| Unit tests | ✅ 182 tests | 7 suites, ASan + UBSan by default |
| Spec conformance | ✅ Complete | Three documented captures replayed byte for byte |
| Parser robustness | ✅ Complete | Control-byte sweep plus 7000 fuzz rounds through the full receive path, under ASan and UBSan |
| Mutation checks | ✅ Complete | Six deliberate regressions - inverted mode bit, wrong size bias, dropped ACEI check, disabled replay guard, always-true MAC comparison, constant-seeded RNG - are each caught by the suite |
| ESPHome config validation | ✅ Complete | Positive and six negative cases |
| ESPHome compile | ✅ CI | `esphome compile` on every change |
| Integration tests | ❌ None | Requires hardware |
| Hardware conformance | ❌ None | See K1 |

Run the tests with:

```sh
./tools/run_native_tests.sh            # all suites
./tools/run_native_tests.sh test_frame # one suite
pio test -e native                     # via PlatformIO
```

---

## Recommended Next Steps

### P0 - Before any deployment
1. Verify against a real actuator (K1). Everything else is downstream of this.
2. Confirm or remove the unverified Velux command IDs (K2).

### P1 - High priority
3. Wire the 2W challenge-response handshake into the ESPHome component (K3).
4. Authenticate position feedback before applying it (K4).
5. Document the fixed-length hook per radio chip (K5).

### P2 - Medium priority
6. Share the crypto sources between the library and the ESPHome component (K6).
7. Add a sensor platform for battery and actuator status.
8. Structured logging (K7).

### P3 - Nice to have
9. Fold in or remove the older ESP32 helper layer (K8).
10. MicroPython implementation.
11. Performance profiling.
