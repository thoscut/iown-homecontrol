# Production Readiness Assessment

## Overview

This document tracks the production readiness status of the iown-homecontrol
project, covering the C++ protocol library (`src/protocol/`), the high-level
controller (`src/IoHomeControl`), the Velux helpers (`src/velux/`) and the
ESPHome integration (`esphome/components/iown_homecontrol/`).

**Current Status: BETA**

The protocol layer is verified against the byte-for-byte captures in `docs/`,
covered by 227 host-run unit tests, and hardened against the receive path being
attacker-controlled. The core path is now also confirmed on **real hardware**:
on a Heltec V4 a physical io-homecontrol actuator pairs, obeys our
open/close/position/ventilation commands (it actually moves), and its own frames
decode byte-correct. The one behaviour still unconfirmed on hardware is the FP1
tilt *direction* on a slatted product - see K1. Coverage is still one board and a
limited set of actuators, so this stays BETA rather than a broad conformance
claim.

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
| V44 | Medium | `library.properties`, `library.json` | The `url` and `homepage` fields pointed at `velocet.github.io/iown-homecontrol`, which 404s - the Pages site is not published. arduino-lint fetches that field, and a dead link blocks Library Manager submission. |
| V45 | Low | `library.json` | Declared RadioLib as `^7.1.2` - a third declaration of the same dependency, at a third version, none of which matched. All three now name 7.7.1 and CI compares them. |
| V46 | Low | `.github/workflows/arduino-lint.yml` | Disabled with `on: workflow_dispatch` and a "TODO Remove", like V15 - and configured with `library-manager: update`, which asserts the library is already in the index. It is not, so that mode could never have passed. Enabled with the specification rules; the library now has the `iown-homecontrol.h` the specification asks for. |
| V47 | Low | Repository | Three tracked Windows executables (1.7 MB) blocked addition to the Arduino Library Manager index. Removed; the directories they sat in now say what each tool was and where to fetch it. `arduino-lint --library-manager submit` reports no errors or warnings. |
| V48 | Medium | `IoHomeControl` (K5) | The packet-length hook had to be installed with a lambda that casts `void*` back to the concrete radio type, naming that type twice; getting the cast wrong is undefined behaviour that compiles cleanly. `use_radio_packet_length(radio)` deduces it instead. |
| V49 | Low | Legacy helpers (K8) | `src/esp32_utils.cpp` and `src/iown_mac.cpp` had no runtime coverage at all. Dropping an unused `<Arduino.h>` made them host-buildable, and `test_legacy_helpers` now checks the frame-length macros against the captures and the protocol layer, the broadcast address, and `iown_crc_calc()` over every length from 1 to 63. |
| V50 | **High** | ESPHome component (K4) | `position_feedback` applied whatever position a frame claimed, with no check at all - anyone in radio range could park a cover's reported state wherever they liked, and a recording of a genuine report replayed forever. Frames are now verified against the system key and the rolling code before anything acts on them, using the protocol layer's own `validate_frame()` and `ReplayGuard`. |
| V51 | Medium | ESPHome component (K6) | The component carried its own CRC and MAC construction because an `external_components` directory cannot reach `src/protocol/`. It no longer carries a second implementation: `tools/sync_esphome_protocol.py` copies the protocol layer in verbatim and CI fails if the copy drifts. ESPHome skips component subdirectories, so the copies sit alongside the component's own sources. |
| V52 | Medium | ESPHome component (K3) | 2W frames went out unauthenticated - there was no handshake at all. `two_way: true` now runs the documented exchange: a 0x3C challenge request, the peer's 0x3D answer, and every command after that signed against the nonce the peer chose. A peer-initiated challenge is answered too. The schema refuses `two_way` without a `system_key`. |
| V53 | Medium | ESPHome component | The hub compiles `iown_cover.cpp` and `iown_sensor.cpp` unconditionally, and those include the cover and sensor components - but `AUTO_LOAD` was empty, so any configuration using only one of the two platforms failed on a missing `esphome/components/sensor/sensor.h`. `example.yaml` uses both, which is why CI never saw it. Both are auto-loaded now and CI compiles a cover-only and a sensor-only configuration. |
| V54 | Low | `IoHomeControl` (K7) | Logging was two macros that expanded to `Serial.printf` or `printf` behind a `verbose_` flag, with severity spelled into the message text as an "Error:" prefix that nothing could filter on, and no way for an application to see the messages at all. `set_log_callback()` now routes every message to the application with its severity as a parameter and a minimum level that drops the rest before formatting. The transmitted-frame hex dump was one call per byte, which only worked because the serial port added no line breaks; it is one `DEBUG` message now. `log()` passed the caller's string as the format string - a message containing a percent sign read arguments that were never passed. The printf attribute on `emit_log()` means the compiler checks all 80 format strings, which it never could while they went through a macro. |
| V55 | **High** | ESPHome component, `AuthenticationManager` | Any 0x3D frame in radio range cancelled a 2W handshake in progress. `verify_challenge_response()` checked the MAC and nothing else, and burned the nonce on failure to stop an attacker grinding guesses - so six arbitrary bytes with command 0x3D, from anyone with a radio, reset the state to IDLE and the genuine answer was then rejected. No key needed. A second io-homecontrol system in the same house did it by accident. The manager now remembers which node it challenged and as whom, and ignores a response between any other pair without touching its state; the component ignores 0x3C/0x3D not addressed to it. |
| V56 | **High** | ESPHome component | With `two_way: true` no incoming 2W frame could ever authenticate. A 2W MAC is bound to the session nonce, and the component called `validate_frame(frame, key)` without one - which that function refuses outright. Every 2W frame was counted as a MAC error and dropped, so position feedback never worked in 2W mode. The session's challenge is now passed, and a 0x3C is checked against the nonce it carries. |
| V57 | **High** | ESPHome component | The component answered a 0x3C challenge request without verifying it, so anyone in radio range could hand this hub a nonce of their choosing and read back the MAC computed over it with the system key - a chosen-input oracle, offered to strangers, on request. A challenge request is now answered only if its own signature holds. |
| V58 | **High** | ESPHome component | The replay check ran only for 1W frames - `is_1w_mode && replay_guard_.accept(...)`, with nothing at all on the 2W branch. A 2W position report was authenticated but never fresh: record one and re-transmit it and it was applied again every time, for as long as the session lasted. That is the attack V50 closed, still open for the 2W half. `ReplayGuard::accept_mac()` already existed for exactly this and `IoHomeControl` already called it; the component now does too. |
| V59 | **High** | `DiscoveryManager`, `iohome_constants.h` | Every field of a Discover Answer was read at the wrong offset. The node type is 16 bits - ten of type, six of sub-type - and was read as `data[0]`, truncating it to the high byte; the manufacturer was read from `data[1]`, which is the type's low half; and `protocol_version` came from `data[2]`, the first byte of the node address, a field the answer does not contain. Against the worked example in `docs/commands.md`, `29 FFC0 XXXXXX 0C CC 0000`, a remote controller from Atlantic was reported as device type 0xFF from manufacturer 0xC0. `DeviceType`'s nineteen values matched neither source and are replaced by `NodeType` with the real ones. See [`docs/VELUX-FORMAT.md`](docs/VELUX-FORMAT.md). |
| V60 | Medium | `src/velux/` (K2) | The six `VELUX_CMD_*` constants at 0x58-0x5D were invented. 0x50-0x57 are four request/answer *pairs* and the six sat among them as unpaired singletons, in a metadata range, four of them claiming to be actuator control - which is command 0x00. Each function they named exists and none is a command: rain is Command Originator 0x02 on an ordinary frame, ventilation is Main Parameter 0xD803 ("Secured Ventilation"), emergency close is Originator 0xFF plus a protection priority, status is the Current access method. Set/reset limitation is real io-homecontrol functionality whose RF command ID is genuinely unknown. |
| V61 | Medium | `src/velux/` | `parse_rain_sensor_status()` looked for command 0x58 with a DRY/RAIN/ERROR byte, so it never matched anything and claimed to observe a "dry" state nothing reports. It now detects what is actually observable: an Execute whose originator is the rain sensor. `create_emergency_close_frame()` built a *rain* close - Originator SENSOR_RAIN - under a name that said otherwise; the two are now separate functions with the right originator each. `detect_model()` derived specific product numbers (GGL_ELECTRIC, FML) from a node type that cannot distinguish them; it is `detect_category()` now. |
| V62 | **High** | `IoHomeControl` 2W pairing (K9) | `pair_device_2w` fired a lone 0x32 key transfer masked against a challenge it generated itself - a nonce the device had never seen - so no real device could unmask the key. It now runs the documented push: 0x31 ask-challenge out, the device's 0x3C challenge in, then the 0x32 masked against *that* nonce, then the device's 0x33 ack. A `KeyReceivedCallback` plus `set_accept_pairing()` add the missing follower side: an incoming 0x31 is answered with a fresh challenge, the 0x32 is unmasked with `AuthenticationManager::recover_2w_key()`, adopted via `set_system_key()`, and acknowledged. A two-instance host test drives the whole exchange and asserts the recovered key equals the pushed key; a mutation that reinstates the self-generated challenge fails it. |
| V63 | Medium | `IoHomeControl` | Two 2W gaps closed. **Pull**: `pull_device_key_2w()` collects a device's existing key - 0x38 launch out, the device's 0x32 back, unmasked against the challenge the 0x38 carried - and surfaces it via the key-received callback without adopting it (it is the peer's key). The follower side answers a 0x38 with its own key. **Framing**: the library now applies the UART start/stop framing on transmit and strips it on receive, the same step the ESPHome component already had - so the Arduino path is on-air-correct too. The RadioLib mock models the framed air; a two-instance pull test and the framed sniffer/fuzz paths cover it. |
| V64 | Medium | `IoHomeControl`, `src/velux/`, ESPHome, docs | Completed the Velux command set for solar roof windows and roller shutters, cross-checked against `rspaargaren/iohomecontrol`. **Ventilation/force**: `ventilate()` sends Main Parameter 0xD803 (secured ventilation) and `force()` sends the observed 0x6400 preset - both confirmed on air from a second implementation; `MP_FORCE` is marked *observed, not spec-derived* since 0x6400 is wire-identical to a 50 % position. **Velux helpers**: `VeluxWindow::create_force_frame()` and the missing `VeluxBlind::create_stop_frame()` (a roller shutter travels, so it needs a stop) round out the frame builders. **Priority**: `set_priority(PriorityLevel)` sets only the ACEI level bits, the clean way to match rspaargaren's User Level 1 (0x43) or the default User Level 2 (0x61) without hand-assembling the byte. **ESPHome**: `ventilate()`/`force()` on the hub, plus a CI-compiled `esphome/example-velux.yaml` with a solar window, a roller shutter and preset buttons. **Docs**: a command / Main Parameter table and an ACEI priority section in [`src/velux/README.md`](src/velux/README.md), the second-implementation corroboration recorded in [`docs/VELUX-FORMAT.md`](docs/VELUX-FORMAT.md). |

| V65 | **High** → fixed | docs, `IoHomeControl`, ESPHome, `ReplayGuard` | A multi-agent expert review (7 lenses, adversarial verification) surfaced twelve confirmed findings; all are fixed. **Key leak (High):** the 0x30 1W key-transfer frame that commit 2e2217b redacted survived in *eight* other places - seven raw-dump lines in `captures/README.md` and one in `velux-frame-analysis.md` - re-exposing the recoverable installation key. All redacted (and a further copy in the framing test vectors was missed here but caught by a second sweep - see V66; history caveat stands). **Crash (Medium):** a 1W-mode node with `accept_pairing_` set dereferenced a null `auth_manager_` on an unauthenticated broadcast 0x31; `is_pairing_frame()`/`handle_pairing_frame()` now refuse to route pairing frames in 1W mode. **Cover (Medium):** the ESPHome 2W cover animated to a position the actuator never received when a command was dropped mid-handshake; `control()` now only arms movement when the send succeeded, like the tilt path. **Pairing timeout (Low):** a stalled handshake stayed stuck and diverted unrelated 0x3C/0x32 frames; `pairing_state_` now expires after 5 s. **Key log (Low):** the ESPHome key-capture printed the recovered key over the network logger with no warning; a loud security warning now precedes it. **Replay doc (Low):** the receive-side `ReplayGuard` is RAM-only and resets on reboot - documented in the header and the threat table, which had over-credited "persisted rolling code". Plus doc self-contradiction (§5/§7 of the frame analysis vs §0), three stale README/checklist claims, and three test-quality gaps (TX framing now asserted at the wire level, the counter-wrap test now crosses the real 2^32 boundary via 32-bit hopper math, and pairing has stranger-frame + key-adoption negative tests). |

| V66 | **High** → fixed | `test/test_phy_framing/` | A second review round (focused re-verification + a repo-wide secret sweep) caught what V65 missed: the same real 0x30 1W key-transfer frame survived as a test vector - both the on-air framed form and the de-framed cleartext - because the sweep behind V65 grepped only space-separated hex, and this file stores it as contiguous lowercase hex. The adversarial verifier reproduced the key recovery from it. Replaced with a **synthetic** 0x30 frame (fabricated address ABCDEF and key 00..0f, valid CRC-16/KERMIT, generated by the repo's own codec) so the framing tests still exercise a 0x30 without embedding a secret. The repo is now swept clean across every hex representation (spaced/contiguous, upper/lower) and both frame directions. Lesson recorded: a partial redaction that greps one representation is how V65's own claim of "verified clean" was wrong. Also added `tools/check_no_key_material.py` + a CI workflow as a structural tripwire. |
| V67 | Medium | `IoHomeControl`, ESPHome, `check_no_key_material.py`, docs | A third review round (6 lenses, adversarial verification) surfaced thirteen confirmed findings; all fixed. **2W push (Medium):** the controller adopted the pushed key immediately after sending the 0x32, ignoring the transmit result and not waiting for the 0x33 ack - a lost 0x32 left controller and device on different keys with no signal. It now checks the send and adopts only on the ack, giving `PUSH_WAIT_ACK` a real purpose. **2W session peer-binding (Medium):** `ensure_2w_session_()` reused a session authenticated with actuator A to command actuator B, whose nonce B never saw - B rejects the frame while the cover animates. Added `AuthenticationManager::is_authenticated_with(peer)` and re-challenge on peer mismatch. **Guard evasion (Medium):** `check_no_key_material.py` silently skipped non-UTF-8 files (a real frame in a file with one stray byte rode through) and missed `|`-separated hex - now decodes leniently (skips only NUL-containing binaries) and widens the separator class. **STOP gating (Low):** the cover STOP path and the auto-STOP froze the estimate even when the STOP was not sent; now gated like OPEN/CLOSE. **Docs (Medium×2 + Low×3):** SECURITY-MODEL wrongly said position feedback skips the MAC (it verifies MAC + replay); HARDWARE-BRINGUP still listed K2 (removed) and K9 (implemented/tested) as open; the 223/225 test counts and a rain-vs-emergency example were corrected. **Tests (+4):** a time seam (`set_pairing_clock_ms`) makes the 5 s pairing timeout testable (mutation-verified), truncated-pairing-frame negative tests, and the push key-adoption path. The ESPHome cover `control()`/STOP gating remains verified only by `esphome compile` + hardware, not the host suite (structural: the suite compiles no ESPHome sources). |

### 🔶 KNOWN Issues (Not Yet Fixed)

| ID | Severity | Component | Description | Recommended Fix |
|----|----------|-----------|-------------|-----------------|
| K1 | Low | `src/velux/` tilt | The core path is now confirmed on real hardware (a Heltec V4): a physical actuator pairs, obeys open/close/position/ventilation, and its own frames decode byte-correct. The blanket "no hardware verification" no longer holds. What is still unconfirmed on hardware is the **direction of Functional Parameter 1** on a tilting product: `create_tilt_frame()` assumes FP1 counts closure the way the Main Parameter does, inferred from the shared scale rather than from a capture, so a blind could tilt the wrong way. Broader coverage (more actuator models, the 2W session end-to-end) is also still thin. | Tilt a slatted product (Velux FML) both ways and capture FP1 - see the "direction of Functional Parameter 1" experiment in [`docs/HARDWARE-BRINGUP.md`](docs/HARDWARE-BRINGUP.md) |
| K8 | Low | `src/esp32_api_spi.cpp` | The SPI register helpers still have no runtime coverage - they need an ESP-IDF SPI host, so a host test cannot reach them. Everything else in the legacy layer is now tested (`test_legacy_helpers`). | Exercise on hardware, or remove |

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
| Device pairing | ✅ 1W + 2W | 2W push (0x31/0x3C/0x32/0x33) and pull (0x38/0x32) both run end to end, host-tested; not yet tried against hardware (K1) |
| Beacon handling | ✅ Complete | |
| Rolling code persistence | ✅ Complete | Block-reserved NVS writes |
| Memory management | ✅ Complete | Destructor, `nothrow`, non-copyable |
| Input validation | ✅ Complete | |
| Logging | ✅ Complete | Severity-tagged, routable to the application, filtered before formatting |

### ESPHome Component

| Feature | Status | Notes |
|---------|--------|-------|
| Radio initialization | ✅ Complete | SX1276/SX1262, CRC and length byte disabled |
| Frame reception | ✅ Complete | ISR driven, CRC verified, length-field checked |
| Cover control | ✅ Complete | Open/Close/Stop/Position |
| Tilt support | ✅ Complete | Via Functional Parameter 1, opt-in |
| 1W authentication | ✅ Complete | With persisted rolling code |
| 2W authentication | ✅ Complete | `two_way: true` runs the 0x3C/0x3D handshake and signs every command against the peer's nonce |
| Position feedback | ✅ Complete | MAC and rolling code verified before anything acts on it; off by default |
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
| Unit tests | ✅ 227 tests | 9 suites, ASan + UBSan by default |
| Spec conformance | ✅ Complete | Three documented captures replayed byte for byte |
| Parser robustness | ✅ Complete | Control-byte sweep plus 7000 fuzz rounds through the full receive path, under ASan and UBSan |
| Mutation checks | ✅ Complete | Eight deliberate regressions - inverted mode bit, wrong size bias, dropped ACEI check, disabled replay guard, always-true MAC comparison, constant-seeded RNG, ignored log level, log message used as its own format string - are each caught by the suite |
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
1. Verify against a real actuator (K1). Everything else is downstream of this,
   including the 2W pairing exchange, which is host-tested end to end but has
   never run against a real device.

### P2 - Medium priority
2. Give the SPI register helpers runtime coverage, or remove them (K8). They
   need an ESP-IDF SPI host, so a host test cannot reach them.

### P3 - Nice to have
3. MicroPython implementation.
4. Performance profiling.

K2, K3, K4, K5, K6, K7, K9 and K10 are closed - see V47 to V62 above. K2 was
resolved without hardware, from the Velux KLF 200 API specification that was
already in this repository; [`docs/VELUX-FORMAT.md`](docs/VELUX-FORMAT.md)
records what that settled, what it contradicted, and what is still open. K9 -
the 2W pairing exchange - is implemented and host-tested (V62), pending only
the hardware confirmation that K1 covers.
