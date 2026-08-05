/**
 * @file board_pins.h
 * @brief Radio wiring for the supported LoRa32 development boards
 *
 * The firmware used to get its pin map from the external `LoRa32` library.
 * That dependency was dropped for three reasons:
 *
 *  1. It ships an example `main.cpp` (defining `radio`, `setup()` and
 *     `loop()`) inside its `src/` directory, so PlatformIO compiles it as
 *     part of the library and the link step fails with "multiple definition
 *     of `setup'" for any project that has its own entry point.
 *  2. It was pulled from an unpinned branch, and its own manifest depends on
 *     RadioLib's `main` branch, so the build was never reproducible.
 *  3. Its board table keys the TTGO v2.1 entry on `ARDUINO_TTGO_LORA32_V21NEW`
 *     while PlatformIO actually defines `ARDUINO_TTGO_LoRa32_v21new`. C macros
 *     are case sensitive, so that board silently fell through to "no radio
 *     defined" and failed to compile.
 *
 * The pin numbers below are the same ones, verified against the Arduino-ESP32
 * variant headers (`variants/<board>/pins_arduino.h`) that PlatformIO selects
 * for each board.
 *
 * ## Adding a board
 *
 * Either add an entry to the table below, or define `IOHC_BOARD_CUSTOM` and
 * supply the pins from your build flags, e.g.
 *
 * @code{.ini}
 * build_flags = -DIOHC_BOARD_CUSTOM
 *               -DIOHC_RADIO_SX127X
 *               -DIOHC_RADIO_CLASS=SX1276
 *               -DIOHC_PIN_CS=18 -DIOHC_PIN_RST=14
 *               -DIOHC_PIN_DIO0=26 -DIOHC_PIN_DIO1=35
 *               -DIOHC_PIN_SCK=5 -DIOHC_PIN_MISO=19 -DIOHC_PIN_MOSI=27
 * @endcode
 */

#ifndef IOHOME_BOARD_PINS_H
#define IOHOME_BOARD_PINS_H

#include <Arduino.h>
#include <SPI.h>

#include <RadioLib.h>

// ---------------------------------------------------------------------------
// Board table
//
// IOHC_RADIO_SX127X / IOHC_RADIO_SX126X select the module family, which
// decides the order RadioLib's Module constructor expects its pins in.
// ---------------------------------------------------------------------------

#if defined(IOHC_BOARD_CUSTOM)
  // Everything comes from build flags; nothing to do here.

#elif defined(ARDUINO_HELTEC_WIFI_LORA_32_V2)
  #define IOHC_BOARD_NAME  "Heltec WiFi LoRa 32 (V2)"
  #define IOHC_RADIO_SX127X
  #define IOHC_RADIO_CLASS SX1276
  #define IOHC_PIN_CS      18
  #define IOHC_PIN_RST     14
  #define IOHC_PIN_DIO0    26
  #define IOHC_PIN_DIO1    35
  #define IOHC_PIN_SCK      5
  #define IOHC_PIN_MISO    19
  #define IOHC_PIN_MOSI    27

#elif defined(ARDUINO_HELTEC_WIFI_LORA_32)
  #define IOHC_BOARD_NAME  "Heltec WiFi LoRa 32 (V1)"
  #define IOHC_RADIO_SX127X
  #define IOHC_RADIO_CLASS SX1276
  #define IOHC_PIN_CS      18
  #define IOHC_PIN_RST     14
  #define IOHC_PIN_DIO0    26
  #define IOHC_PIN_DIO1    33
  #define IOHC_PIN_SCK      5
  #define IOHC_PIN_MISO    19
  #define IOHC_PIN_MOSI    27

// PlatformIO spells this one `ARDUINO_TTGO_LoRa32_v21new`. Accept the
// upper-case variant too so a board definition that changes its mind about
// capitalisation does not silently drop off the table.
#elif defined(ARDUINO_TTGO_LoRa32_v21new) || defined(ARDUINO_TTGO_LORA32_V21NEW)
  #define IOHC_BOARD_NAME  "TTGO LoRa32 v2.1.6"
  #define IOHC_RADIO_SX127X
  #define IOHC_RADIO_CLASS SX1276
  #define IOHC_PIN_CS      18
  #define IOHC_PIN_RST     23
  #define IOHC_PIN_DIO0    26
  #define IOHC_PIN_DIO1    33
  #define IOHC_PIN_SCK      5
  #define IOHC_PIN_MISO    19
  #define IOHC_PIN_MOSI    27

#elif defined(ARDUINO_TTGO_LORA32_V1) || defined(ARDUINO_TTGO_LoRa32_v1)
  #define IOHC_BOARD_NAME  "TTGO LoRa32 v1"
  #define IOHC_RADIO_SX127X
  #define IOHC_RADIO_CLASS SX1276
  #define IOHC_PIN_CS      18
  #define IOHC_PIN_RST     14
  #define IOHC_PIN_DIO0    26
  #define IOHC_PIN_DIO1    26
  #define IOHC_PIN_SCK      5
  #define IOHC_PIN_MISO    19
  #define IOHC_PIN_MOSI    27

#elif defined(ARDUINO_TTGO_LORA32_V2) || defined(ARDUINO_TTGO_LoRa32_V2)
  #define IOHC_BOARD_NAME  "TTGO LoRa32 v2"
  #define IOHC_RADIO_SX127X
  #define IOHC_RADIO_CLASS SX1276
  #define IOHC_PIN_CS      18
  #define IOHC_PIN_RST     12
  #define IOHC_PIN_DIO0    26
  #define IOHC_PIN_DIO1    26
  #define IOHC_PIN_SCK      5
  #define IOHC_PIN_MISO    19
  #define IOHC_PIN_MOSI    27

#elif defined(ARDUINO_TBEAM) || defined(ARDUINO_T_BEAM)
  #define IOHC_BOARD_NAME  "LilyGO T-Beam"
  #define IOHC_RADIO_SX127X
  #define IOHC_RADIO_CLASS SX1276
  #define IOHC_PIN_CS      18
  #define IOHC_PIN_RST     23
  #define IOHC_PIN_DIO0    26
  #define IOHC_PIN_DIO1    33
  #define IOHC_PIN_SCK      5
  #define IOHC_PIN_MISO    19
  #define IOHC_PIN_MOSI    27

// SX1262 boards. Untested on hardware for io-homecontrol; the pin map matches
// the vendor schematics but nobody has confirmed a capture from one of these.
#elif defined(ARDUINO_HELTEC_WIFI_LORA_32_V3)
  #define IOHC_BOARD_NAME  "Heltec WiFi LoRa 32 (V3)"
  #define IOHC_RADIO_SX126X
  #define IOHC_RADIO_CLASS SX1262
  #define IOHC_PIN_CS       8
  #define IOHC_PIN_RST     12
  #define IOHC_PIN_BUSY    13
  #define IOHC_PIN_DIO1    14
  #define IOHC_PIN_SCK      9
  #define IOHC_PIN_MISO    11
  #define IOHC_PIN_MOSI    10

#elif defined(ARDUINO_HELTEC_WIRELESS_STICK_V3)
  #define IOHC_BOARD_NAME  "Heltec Wireless Stick (V3)"
  #define IOHC_RADIO_SX126X
  #define IOHC_RADIO_CLASS SX1262
  #define IOHC_PIN_CS       8
  #define IOHC_PIN_RST     12
  #define IOHC_PIN_BUSY    13
  #define IOHC_PIN_DIO1    14
  #define IOHC_PIN_SCK      9
  #define IOHC_PIN_MISO    11
  #define IOHC_PIN_MOSI    10

#else
  #error "iown-homecontrol: unknown board. Add it to src/board_pins.h, or build with -DIOHC_BOARD_CUSTOM and supply IOHC_RADIO_CLASS / IOHC_PIN_* yourself."
#endif

#if !defined(IOHC_BOARD_NAME)
  #define IOHC_BOARD_NAME "custom"
#endif

// ---------------------------------------------------------------------------
// Sanity checks
//
// A half-filled custom configuration is easier to debug at compile time than
// as a radio that never answers.
// ---------------------------------------------------------------------------

#if !defined(IOHC_RADIO_CLASS)
  #error "IOHC_RADIO_CLASS is not defined (e.g. SX1276). See src/board_pins.h."
#endif
#if !defined(IOHC_RADIO_SX127X) && !defined(IOHC_RADIO_SX126X)
  #error "Define IOHC_RADIO_SX127X or IOHC_RADIO_SX126X to select the module family."
#endif
#if defined(IOHC_RADIO_SX127X) && defined(IOHC_RADIO_SX126X)
  #error "IOHC_RADIO_SX127X and IOHC_RADIO_SX126X are mutually exclusive."
#endif
#if !defined(IOHC_PIN_CS) || !defined(IOHC_PIN_RST) || \
    !defined(IOHC_PIN_SCK) || !defined(IOHC_PIN_MISO) || !defined(IOHC_PIN_MOSI)
  #error "Incomplete board pin map: need IOHC_PIN_CS, _RST, _SCK, _MISO and _MOSI."
#endif
#if defined(IOHC_RADIO_SX127X) && (!defined(IOHC_PIN_DIO0) || !defined(IOHC_PIN_DIO1))
  #error "SX127x needs IOHC_PIN_DIO0 (packet interrupt) and IOHC_PIN_DIO1."
#endif
#if defined(IOHC_RADIO_SX126X) && (!defined(IOHC_PIN_BUSY) || !defined(IOHC_PIN_DIO1))
  #error "SX126x needs IOHC_PIN_BUSY and IOHC_PIN_DIO1 (packet interrupt)."
#endif

// ---------------------------------------------------------------------------
// Module construction
//
// RadioLib's Module constructor is Module(cs, irq, rst, gpio), but the two
// families disagree about which physical pin plays which role:
//
//   SX127x: irq = DIO0 (PacketSent / PayloadReady), gpio = DIO1
//   SX126x: irq = DIO1 (the only interrupt line broken out), gpio = BUSY
//
// The LoRa32 library passed its "IO0" slot as `irq` for both families, which
// handed SX126x boards BUSY as the interrupt line and DIO1 as the busy line -
// exactly backwards. Spelling the mapping out per family avoids repeating that.
// ---------------------------------------------------------------------------

#if defined(IOHC_RADIO_SX127X)
  #define IOHC_MODULE_ARGS IOHC_PIN_CS, IOHC_PIN_DIO0, IOHC_PIN_RST, IOHC_PIN_DIO1
#else
  #define IOHC_MODULE_ARGS IOHC_PIN_CS, IOHC_PIN_DIO1, IOHC_PIN_RST, IOHC_PIN_BUSY
#endif

/// Convenience macro: `IOHC_RADIO_CLASS radio = IOHC_MAKE_RADIO();`
#define IOHC_MAKE_RADIO() new Module(IOHC_MODULE_ARGS)

/**
 * @brief Bind the default SPI bus to this board's radio pins.
 *
 * Both supported variant headers already remap `SS`/`SCK`/`MISO`/`MOSI` to the
 * radio, so RadioLib's implicit `SPI.begin()` happens to work there. Boards
 * whose variant leaves the defaults alone need the explicit form, and calling
 * it costs nothing when the pins already match - so always call it before
 * `radio.beginFSK()` rather than relying on the variant.
 */
inline void iohc_board_spi_begin() {
  SPI.begin(IOHC_PIN_SCK, IOHC_PIN_MISO, IOHC_PIN_MOSI, IOHC_PIN_CS);
}

#endif  // IOHOME_BOARD_PINS_H
