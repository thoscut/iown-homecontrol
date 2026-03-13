/**
 * @file iown_homecontrol.h
 * @brief ESPHome component for io-homecontrol protocol
 *
 * Hub component that initializes the radio module and provides
 * io-homecontrol frame reception, parsing, and transmission.
 *
 * Uses RadioLib for radio abstraction supporting SX1276/SX1262 modules.
 *
 * Protocol: io-homecontrol (iohc) - 868 MHz FSK
 * - 3 channels: 868.25, 868.95, 869.85 MHz
 * - 38.4 kbps, 19.2 kHz deviation, NRZ encoding
 * - Sync word: 0x57FD99
 * - CRC-16/KERMIT
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include <RadioLib.h>
#include <SPI.h>
#include <vector>

#if defined(ESP32)
#include <atomic>
#endif

namespace esphome {
namespace iown_homecontrol {

// io-homecontrol channel frequencies (MHz)
static const float IOHC_CHANNEL_1 = 868.25f;  // 2W only
static const float IOHC_CHANNEL_2 = 868.95f;  // 1W/2W (default)
static const float IOHC_CHANNEL_3 = 869.85f;  // 2W only

// Physical layer parameters
static const uint8_t IOHC_SYNC_WORD[] = {0x57, 0xFD, 0x99};
static const uint8_t IOHC_SYNC_WORD_LEN = 3;
static const uint16_t IOHC_PREAMBLE_BITS = 512;

// Protocol mode flags (Control Byte 0, bit 5)
static const uint8_t IOHC_MODE_2W = 0x00;
static const uint8_t IOHC_MODE_1W = 0x20;

// Command IDs
static const uint8_t IOHC_CMD_EXECUTE = 0x00;
static const uint8_t IOHC_CMD_ACTIVATE_MODE = 0x01;

// Main parameter values for Command 0x00
static const uint16_t IOHC_PARAM_OPEN = 0x0000;   // Open / Min / On
static const uint16_t IOHC_PARAM_CLOSE = 0xC800;  // Close / Max / Off
static const uint16_t IOHC_PARAM_STOP = 0xD200;   // Stop / Current position

// Maximum frame size
static const size_t IOHC_MAX_FRAME_SIZE = 64;

// Minimum frame size: ctrl0(1) + ctrl1(1) + dest(3) + src(3) + cmd(1) + crc(2) = 11
static const size_t IOHC_MIN_FRAME_SIZE = 11;

enum RadioType {
  RADIO_SX1276 = 0,
  RADIO_SX1262 = 1,
};

// Forward declaration
class IOWNCover;

/**
 * @class IOWNHomeControlComponent
 * @brief Main hub component for io-homecontrol radio communication.
 *
 * Initializes the radio module, configures io-homecontrol physical layer
 * parameters, receives and parses incoming frames, and provides methods
 * for transmitting cover commands.
 */
class IOWNHomeControlComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_cs_pin(int pin) { this->cs_pin_ = pin; }
  void set_rst_pin(int pin) { this->rst_pin_ = pin; }
  void set_dio0_pin(int pin) { this->dio0_pin_ = pin; }
  void set_dio1_pin(int pin) { this->dio1_pin_ = pin; }
  void set_frequency(float freq) { this->frequency_ = freq; }
  void set_radio_type(RadioType type) { this->radio_type_ = type; }
  void set_source_address(uint32_t addr) { this->source_address_ = addr; }
  void set_sck_pin(int pin) { this->sck_pin_ = pin; }
  void set_mosi_pin(int pin) { this->mosi_pin_ = pin; }
  void set_miso_pin(int pin) { this->miso_pin_ = pin; }
  void set_system_key(const std::vector<uint8_t> &key);
  void set_encryption_enabled(bool enabled) { this->encryption_enabled_ = enabled; }

  void register_cover(IOWNCover *cover) { this->covers_.push_back(cover); }

  /** Send a raw frame over the radio. */
  bool send_frame(const uint8_t *data, size_t len);

  /** Send a cover control command (2W plain or 1W encrypted mode). */
  bool send_cover_command(uint32_t target_address, uint8_t command, uint16_t main_param);

  /** Compute CRC-16/KERMIT over the given data. */
  static uint16_t compute_crc(const uint8_t *data, size_t len);

 protected:
  int cs_pin_{-1};
  int rst_pin_{-1};
  int dio0_pin_{-1};
  int dio1_pin_{-1};
  int sck_pin_{-1};
  int mosi_pin_{-1};
  int miso_pin_{-1};
  float frequency_{IOHC_CHANNEL_2};
  RadioType radio_type_{RADIO_SX1276};
  uint32_t source_address_{0x1A380B};

  Module *radio_module_{nullptr};
  PhysicalLayer *phy_{nullptr};

  std::vector<IOWNCover *> covers_;

  uint8_t system_key_[16] = {0};
  bool encryption_enabled_{false};
  uint16_t rolling_code_{0};

#if defined(ESP32)
  static std::atomic<bool> packet_flag_;
  static void IRAM_ATTR packet_isr_() { packet_flag_.store(true, std::memory_order_release); }
#else
  static volatile bool packet_flag_;
  static void packet_isr_() { packet_flag_ = true; }
#endif

  /** Configure the radio physical layer for io-homecontrol. */
  int16_t configure_phy_layer_();

  /** Check for and process received frames. */
  void receive_frame_();

  /** Parse a received io-homecontrol frame. */
  void parse_frame_(const uint8_t *data, size_t len);

  /** Compute 6-byte HMAC for 1W mode using AES-128-ECB. */
  bool compute_hmac_(const uint8_t *frame_data, size_t data_len,
                     const uint8_t rolling_code[2], uint8_t hmac_out[6]);
};

}  // namespace iown_homecontrol
}  // namespace esphome
