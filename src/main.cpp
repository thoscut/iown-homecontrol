/**
 * @file main.cpp
 * @brief iown-homecontrol firmware entry point
 * @author Velocet
 *
 * io-homecontrol (Somfy, Velux, etc.) implementation for LoRa32 boards.
 *
 * This sketch brings up the radio, configures the io-homecontrol physical
 * layer and listens for frames, printing every one it accepts. Set
 * IOHC_NODE_ID and IOHC_SYSTEM_KEY to match your installation before use.
 *
 * MIT License
 * Copyright (c) Velocet
 */

#include <Arduino.h>
#include <SPI.h>

#include <RadioLib.h>

#include "IoHomeControl.h"
#include "board_pins.h"
#include "protocol/iohome_nvs_store.h"

// RadioLib: load the module using this board's pin map (src/board_pins.h).
IOHC_RADIO_CLASS radio = IOHC_MAKE_RADIO();

// RadioLib: common layer pointer shared with the protocol stack.
PhysicalLayer* phy = static_cast<PhysicalLayer*>(&radio);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// This controller's node address.
static const uint8_t IOHC_NODE_ID[iohome::NODE_ID_SIZE] = {0x1A, 0x38, 0x0B};

/// System key shared with the paired actuators.
///
/// Replace this with your own key. A key of all zeroes authenticates nothing
/// useful - see docs/SECURITY-MODEL.md for how to obtain or generate one.
static const uint8_t IOHC_SYSTEM_KEY[iohome::AES_KEY_SIZE] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/// Operate in 1W (send-only, sequence-number authenticated) mode.
static const bool IOHC_USE_1W = true;

/// Sweep the three io-homecontrol channels until frames appear.
///
/// The commonest reason a listener hears nothing is that the traffic is on one
/// of the other two channels. Rather than make that a guess, sit on each in
/// turn until something arrives, then stay there. Set to false to pin the
/// listener to IOHC_START_CHANNEL.
static const bool IOHC_SCAN_CHANNELS = true;

/// How long to give a channel before moving on, in milliseconds.
static const uint32_t IOHC_SCAN_DWELL_MS = 8000;

/// Channel to start on, and the only one used when scanning is off.
static const float IOHC_START_CHANNEL = iohome::FREQUENCY_CHANNEL_2;

// ---------------------------------------------------------------------------

static iohome::IoHomeControl controller(phy);

#if defined(ARDUINO_ARCH_ESP32)
// Persist the rolling code so receivers keep accepting our frames after a
// reboot. Writes are batched by IoHomeControl to spare the flash.
static iohome::NvsRollingCodeStore rolling_code_store;
#endif

static void print_line(const char* line) {
  Serial.println(line);
}

static void on_frame(const iohome::frame::IoFrame* frame, int16_t rssi, float snr) {
  Serial.printf("[RX] %s cmd=0x%02X from %02X%02X%02X rssi=%d snr=%.1f\n",
                frame->is_1w_mode ? "1W" : "2W",
                frame->command_id,
                frame->src_node[0], frame->src_node[1], frame->src_node[2],
                rssi, snr);

  iohome::frame::print_frame(frame, print_line);
}

/**
 * Print every packet the radio delivers, before the protocol layer judges it.
 *
 * The line is plain hex so it can be pasted - or piped from the serial log -
 * straight into the decoder:
 *
 *   ./scripts/Iown-IoHexFrameParser.py f800 00007f ...
 *   grep '^\[RAW\]' capture.log | cut -d' ' -f2 > frames.txt
 *   ./scripts/Iown-IoHexFrameParser.py -f frames.txt
 *
 * Frames that fail their CRC, their MAC or the replay check never reach
 * on_frame(), and those are frequently the interesting ones: traffic from
 * actuators this controller is not paired with, or commands the library does
 * not model yet.
 */
// Not volatile: on_raw_frame() is called from check_received() in loop(), not
// from the packet ISR. Marking it volatile only cost a C++20 deprecation
// warning for the increment.
static uint32_t g_raw_frames = 0;

static void on_raw_frame(const uint8_t* data, size_t len, int16_t rssi, float snr, void* ctx) {
  (void) ctx;
  g_raw_frames++;
  Serial.print(F("[RAW] "));
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print(data[i], HEX);
  }
  Serial.printf(" rssi=%d snr=%.1f len=%u\n", rssi, snr, static_cast<unsigned>(len));
}

/// Report the pin map, so a radio that does not answer points at its own cause.
static void print_pin_map() {
  Serial.println(F("[PINS] board: " IOHC_BOARD_NAME));
  Serial.printf("[PINS] CS=%d RST=%d SCK=%d MISO=%d MOSI=%d\n",
                IOHC_PIN_CS, IOHC_PIN_RST, IOHC_PIN_SCK, IOHC_PIN_MISO, IOHC_PIN_MOSI);
#if defined(IOHC_RADIO_SX127X)
  Serial.printf("[PINS] DIO0=%d DIO1=%d (SX127x)\n", IOHC_PIN_DIO0, IOHC_PIN_DIO1);
#else
  Serial.printf("[PINS] BUSY=%d DIO1=%d (SX126x)\n", IOHC_PIN_BUSY, IOHC_PIN_DIO1);
#endif
}

static void halt(const char* stage, int state) {
  Serial.printf("[FATAL] %s failed: %d\n", stage, state);

  if (state == RADIOLIB_ERR_CHIP_NOT_FOUND) {
    // By far the most common first-bring-up failure, and it has exactly one
    // cause worth checking first.
    Serial.println(F("[FATAL] The radio did not answer over SPI."));
    Serial.println(F("[FATAL] Almost always a pin map that does not match this board."));
    print_pin_map();
    Serial.println(F("[FATAL] Correct them in src/board_pins.h, or build with"));
    Serial.println(F("[FATAL] -DIOHC_BOARD_CUSTOM and the IOHC_PIN_* flags."));
    Serial.println(F("[FATAL] See docs/HARDWARE-BRINGUP.md."));
  }

  while (true) {
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("iown-homecontrol starting"));
  print_pin_map();

  // Bind SPI to the radio pins before RadioLib touches the bus.
  iohc_board_spi_begin();

  int16_t state = radio.beginFSK();
  if (state != RADIOLIB_ERR_NONE) {
    halt("radio.beginFSK", state);
  }

  // RadioLib's FSK defaults add a length byte and its own CRC, neither of
  // which io-homecontrol uses. Both calls live on the concrete radio class,
  // so they cannot be made through the PhysicalLayer pointer.
  state = radio.setCRC(false);
  if (state != RADIOLIB_ERR_NONE) {
    halt("radio.setCRC", state);
  }

  state = radio.fixedPacketLengthMode(iohome::FRAME_MAX_SIZE);
  if (state != RADIOLIB_ERR_NONE) {
    halt("radio.fixedPacketLengthMode", state);
  }

  controller.set_verbose(true);

#if defined(ARDUINO_ARCH_ESP32)
  // Must be set before begin() so the stored counter is picked up.
  controller.set_rolling_code_store(&rolling_code_store);
#endif

  if (!controller.begin(IOHC_NODE_ID, IOHC_SYSTEM_KEY, IOHC_USE_1W)) {
    halt("controller.begin", 0);
  }

  state = controller.configure_radio(IOHC_START_CHANNEL);
  if (state != RADIOLIB_ERR_NONE) {
    halt("configure_radio", state);
  }

  // Install the sniffer before receiving starts, so nothing is missed.
  controller.set_raw_frame_callback(on_raw_frame);

  state = controller.start_receive(on_frame);
  if (state != RADIOLIB_ERR_NONE) {
    halt("start_receive", state);
  }

  Serial.printf("[PHY] RSSI (dBm): %d\n", controller.get_rssi());
  Serial.printf("[PHY] channel: %.2f MHz\n", IOHC_START_CHANNEL);
  if (IOHC_SCAN_CHANNELS) {
    Serial.println(F("[PHY] scanning all three channels until a frame arrives"));
  }
  Serial.println(F("Listening for io-homecontrol frames"));
}

/**
 * Move to the next channel if this one has been silent long enough.
 *
 * Stops as soon as any frame arrives - including one that fails its CRC,
 * because even a broken frame proves the channel carries traffic and the PHY
 * settings are close enough to demodulate it.
 */
static void scan_channels(uint32_t now) {
  static const float channels[] = {
    iohome::FREQUENCY_CHANNEL_1,
    iohome::FREQUENCY_CHANNEL_2,
    iohome::FREQUENCY_CHANNEL_3,
  };
  static size_t index = 1;          // FREQUENCY_CHANNEL_2, the default start
  static uint32_t switched_at = 0;
  static bool done = false;

  if (done) {
    return;
  }

  if (g_raw_frames > 0) {
    done = true;
    Serial.printf("[PHY] traffic on %.2f MHz - staying here\n", channels[index]);
    return;
  }

  if (now - switched_at < IOHC_SCAN_DWELL_MS) {
    return;
  }
  switched_at = now;

  index = (index + 1) % (sizeof(channels) / sizeof(channels[0]));

  controller.stop_receive();
  const int16_t state = controller.configure_radio(channels[index]);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[PHY] could not tune %.2f MHz: %d\n", channels[index], state);
    return;
  }
  if (controller.start_receive(on_frame) != RADIOLIB_ERR_NONE) {
    Serial.println(F("[PHY] could not resume receiving after retuning"));
    return;
  }
  Serial.printf("[PHY] nothing heard, trying %.2f MHz\n", channels[index]);
}

void loop() {
  iohome::frame::IoFrame frame;
  controller.check_received(&frame);

  const uint32_t now_ms = millis();

  if (IOHC_SCAN_CHANNELS) {
    scan_channels(now_ms);
  }

  if (!IOHC_USE_1W) {
    controller.update_frequency_hopping();
  }

  // Periodically report how the receive path is doing.
  static uint32_t last_report = 0;
  if (now_ms - last_report >= 30000) {
    last_report = now_ms;
    const iohome::RxStats& stats = controller.rx_stats();
    Serial.printf("[RX] received=%lu accepted=%lu crc=%lu mac=%lu replay=%lu plain=%lu malformed=%lu\n",
                  static_cast<unsigned long>(stats.received),
                  static_cast<unsigned long>(stats.accepted),
                  static_cast<unsigned long>(stats.crc_failures),
                  static_cast<unsigned long>(stats.mac_failures),
                  static_cast<unsigned long>(stats.replays),
                  static_cast<unsigned long>(stats.unauthenticated),
                  static_cast<unsigned long>(stats.malformed));
  }
}
