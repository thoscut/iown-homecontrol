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

static void halt(const char* stage, int state) {
  Serial.printf("[FATAL] %s failed: %d\n", stage, state);
  while (true) {
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("iown-homecontrol starting"));
  Serial.println(F("board: " IOHC_BOARD_NAME));

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

  state = controller.configure_radio(iohome::FREQUENCY_CHANNEL_2);
  if (state != RADIOLIB_ERR_NONE) {
    halt("configure_radio", state);
  }

  state = controller.start_receive(on_frame);
  if (state != RADIOLIB_ERR_NONE) {
    halt("start_receive", state);
  }

  Serial.printf("[PHY] RSSI (dBm): %d\n", controller.get_rssi());
  Serial.println(F("Listening for io-homecontrol frames"));
}

void loop() {
  iohome::frame::IoFrame frame;
  controller.check_received(&frame);

  if (!IOHC_USE_1W) {
    controller.update_frequency_hopping();
  }

  // Periodically report how the receive path is doing.
  static uint32_t last_report = 0;
  const uint32_t now = millis();
  if (now - last_report >= 30000) {
    last_report = now;
    const iohome::RxStats& stats = controller.rx_stats();
    Serial.printf("[RX] accepted=%lu crc=%lu mac=%lu replay=%lu plain=%lu malformed=%lu\n",
                  static_cast<unsigned long>(stats.accepted),
                  static_cast<unsigned long>(stats.crc_failures),
                  static_cast<unsigned long>(stats.mac_failures),
                  static_cast<unsigned long>(stats.replays),
                  static_cast<unsigned long>(stats.unauthenticated),
                  static_cast<unsigned long>(stats.malformed));
  }
}
