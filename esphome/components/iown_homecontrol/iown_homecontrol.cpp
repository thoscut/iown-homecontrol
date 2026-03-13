/**
 * @file iown_homecontrol.cpp
 * @brief ESPHome component for io-homecontrol protocol - implementation
 */

#include "iown_homecontrol.h"

namespace esphome {
namespace iown_homecontrol {

static const char *const TAG = "iown_homecontrol";

#if defined(ESP32)
std::atomic<bool> IOWNHomeControlComponent::packet_flag_{false};
#else
volatile bool IOWNHomeControlComponent::packet_flag_ = false;
#endif

void IOWNHomeControlComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up io-homecontrol...");

  // Initialize SPI with custom pins if configured
  if (this->sck_pin_ >= 0 && this->miso_pin_ >= 0 && this->mosi_pin_ >= 0) {
    SPI.begin(this->sck_pin_, this->miso_pin_, this->mosi_pin_);
    ESP_LOGD(TAG, "SPI initialized: SCK=%d, MISO=%d, MOSI=%d",
             this->sck_pin_, this->miso_pin_, this->mosi_pin_);
  }

  // Create RadioLib module with pin configuration
  this->radio_module_ = new Module(this->cs_pin_, this->dio0_pin_,
                                   this->rst_pin_, this->dio1_pin_);

  int16_t state = RADIOLIB_ERR_UNKNOWN;

  // Initialize the radio based on configured type
  switch (this->radio_type_) {
    case RADIO_SX1276: {
      auto *radio = new SX1276(this->radio_module_);
      state = radio->beginFSK();
      this->phy_ = radio;
      ESP_LOGD(TAG, "Radio type: SX1276");
      break;
    }
    case RADIO_SX1262: {
      auto *radio = new SX1262(this->radio_module_);
      state = radio->beginFSK();
      this->phy_ = radio;
      ESP_LOGD(TAG, "Radio type: SX1262");
      break;
    }
    default:
      ESP_LOGE(TAG, "Unknown radio type: %d", this->radio_type_);
      this->mark_failed();
      return;
  }

  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Radio initialization failed: %d", state);
    this->mark_failed();
    return;
  }

  // Configure physical layer for io-homecontrol protocol
  state = this->configure_phy_layer_();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Physical layer configuration failed: %d", state);
    this->mark_failed();
    return;
  }

  // Set up interrupt-driven receive
  this->phy_->setPacketReceivedAction(packet_isr_);

  // Start receiving
  state = this->phy_->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "Failed to start receive mode: %d", state);
  }

  ESP_LOGI(TAG, "io-homecontrol initialized on %.2f MHz", this->frequency_);
}

void IOWNHomeControlComponent::loop() {
  this->receive_frame_();
}

void IOWNHomeControlComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "io-homecontrol:");
  ESP_LOGCONFIG(TAG, "  CS Pin: %d", this->cs_pin_);
  ESP_LOGCONFIG(TAG, "  RST Pin: %d", this->rst_pin_);
  ESP_LOGCONFIG(TAG, "  DIO0 Pin: %d", this->dio0_pin_);
  ESP_LOGCONFIG(TAG, "  DIO1 Pin: %d", this->dio1_pin_);
  if (this->sck_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  SCK Pin: %d", this->sck_pin_);
    ESP_LOGCONFIG(TAG, "  MOSI Pin: %d", this->mosi_pin_);
    ESP_LOGCONFIG(TAG, "  MISO Pin: %d", this->miso_pin_);
  }
  ESP_LOGCONFIG(TAG, "  Frequency: %.2f MHz", this->frequency_);
  ESP_LOGCONFIG(TAG, "  Radio Type: %s",
                this->radio_type_ == RADIO_SX1276 ? "SX1276" : "SX1262");
  ESP_LOGCONFIG(TAG, "  Source Address: 0x%06X",
                static_cast<unsigned int>(this->source_address_));
}

int16_t IOWNHomeControlComponent::configure_phy_layer_() {
  int16_t state;

  // Set frequency
  state = this->phy_->setFrequency(this->frequency_);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set frequency: %d", state);
    return state;
  }

  // Set FSK data rate: 38.4 kbps bitrate, 19.2 kHz deviation
  DataRate_t dr;
  dr.fsk.bitRate = 38400.0f;
  dr.fsk.freqDev = 19200.0f;
  state = this->phy_->setDataRate(dr);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set data rate: %d", state);
    return state;
  }

  // No data shaping
  state = this->phy_->setDataShaping(RADIOLIB_SHAPING_NONE);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set data shaping: %d", state);
    return state;
  }

  // NRZ encoding
  state = this->phy_->setEncoding(RADIOLIB_ENCODING_NRZ);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set encoding: %d", state);
    return state;
  }

  // Set sync word: 0x57FD99
  uint8_t sync_word[IOHC_SYNC_WORD_LEN];
  memcpy(sync_word, IOHC_SYNC_WORD, IOHC_SYNC_WORD_LEN);
  state = this->phy_->setSyncWord(sync_word, IOHC_SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set sync word: %d", state);
    return state;
  }

  // Set preamble length (512 bits = 64 bytes)
  state = this->phy_->setPreambleLength(IOHC_PREAMBLE_BITS / 8);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set preamble length: %d", state);
    return state;
  }

  // Set Tx output power (start at 20 dBm, decrease until supported)
  int8_t pwr = 20;
  do {
    state = this->phy_->setOutputPower(pwr);
  } while (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER && --pwr >= -3);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "Could not set output power: %d", state);
  }

  return RADIOLIB_ERR_NONE;
}

uint16_t IOWNHomeControlComponent::compute_crc(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0x8408;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void IOWNHomeControlComponent::receive_frame_() {
#if defined(ESP32)
  if (!packet_flag_.load(std::memory_order_acquire)) {
    return;
  }
  packet_flag_.store(false, std::memory_order_release);
#else
  if (!packet_flag_) {
    return;
  }
  packet_flag_ = false;
#endif

  size_t len = this->phy_->getPacketLength();
  if (len == 0 || len > IOHC_MAX_FRAME_SIZE) {
    this->phy_->startReceive();
    return;
  }

  uint8_t data[IOHC_MAX_FRAME_SIZE];
  int16_t state = this->phy_->readData(data, len);

  if (state == RADIOLIB_ERR_NONE) {
    this->parse_frame_(data, len);
  } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
    ESP_LOGW(TAG, "Receive error: %d", state);
  }

  // Restart receiving
  this->phy_->startReceive();
}

void IOWNHomeControlComponent::parse_frame_(const uint8_t *data, size_t len) {
  if (len < IOHC_MIN_FRAME_SIZE) {
    ESP_LOGW(TAG, "Frame too short: %u bytes", static_cast<unsigned>(len));
    return;
  }

  // Parse Control Byte 0
  uint8_t ctrl0 = data[0];
  uint8_t ctrl1 = data[1];
  uint8_t order = (ctrl0 >> 6) & 0x03;
  bool is_1w = (ctrl0 & 0x20) != 0;
  uint8_t frame_size = ctrl0 & 0x1F;

  // Parse addresses (3 bytes each, big-endian)
  uint32_t dest_addr = ((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 8) | data[4];
  uint32_t src_addr = ((uint32_t) data[5] << 16) | ((uint32_t) data[6] << 8) | data[7];

  // Parse command
  uint8_t cmd = data[8];

  // Verify CRC (last 2 bytes, little-endian)
  if (len >= 4) {
    uint16_t received_crc = data[len - 2] | (data[len - 1] << 8);
    uint16_t calculated_crc = compute_crc(data, len - 2);

    if (received_crc != calculated_crc) {
      ESP_LOGW(TAG, "CRC mismatch: received=0x%04X calculated=0x%04X",
               received_crc, calculated_crc);
      return;
    }
  }

  // Log frame details
  ESP_LOGI(TAG, "Frame: mode=%s order=%d src=0x%06X dst=0x%06X cmd=0x%02X size=%d",
           is_1w ? "1W" : "2W", order,
           static_cast<unsigned int>(src_addr),
           static_cast<unsigned int>(dest_addr),
           cmd, frame_size);

  // Log raw frame data at debug level
  std::string hex_str;
  hex_str.reserve(len * 3);
  for (size_t i = 0; i < len; i++) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X ", data[i]);
    hex_str += buf;
  }
  ESP_LOGD(TAG, "Raw: %s", hex_str.c_str());

  // Parse cover-specific commands
  if (cmd == IOHC_CMD_EXECUTE && len > 10) {
    uint16_t main_param = (data[9] << 8) | data[10];

    const char *action = "unknown";
    if (main_param == IOHC_PARAM_OPEN) {
      action = "OPEN";
    } else if (main_param == IOHC_PARAM_CLOSE) {
      action = "CLOSE";
    } else if (main_param == IOHC_PARAM_STOP) {
      action = "STOP";
    }

    ESP_LOGI(TAG, "Cover command: %s (param=0x%04X)", action, main_param);
  }
}

bool IOWNHomeControlComponent::send_frame(const uint8_t *data, size_t len) {
  if (this->phy_ == nullptr) {
    ESP_LOGE(TAG, "Radio not initialized");
    return false;
  }

  // Clear interrupt flag and stop receiving
  this->phy_->clearPacketReceivedAction();

  int16_t state = this->phy_->transmit(const_cast<uint8_t *>(data), len);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Transmit failed: %d", state);
  }

  // Restore receive mode
  this->phy_->setPacketReceivedAction(packet_isr_);
  this->phy_->startReceive();

  return state == RADIOLIB_ERR_NONE;
}

bool IOWNHomeControlComponent::send_cover_command(uint32_t target_address,
                                                  uint8_t command,
                                                  uint16_t main_param) {
  /*
   * Build a 2W io-homecontrol frame:
   *
   * Byte 0:     Control Byte 0 (order=00, mode=2W, size)
   * Byte 1:     Control Byte 1 (0x00)
   * Byte 2-4:   Destination NodeID (3 bytes, big-endian)
   * Byte 5-7:   Source NodeID (3 bytes, big-endian)
   * Byte 8:     Command ID
   * Byte 9:     Originator (0x01 = user)
   * Byte 10:    ACEI flags (0x00)
   * Byte 11-12: Main Parameter (2 bytes, big-endian)
   * Byte 13:    Functional Parameter 1 (0x00)
   * Byte 14:    Functional Parameter 2 (0x00)
   * Byte 15-16: CRC-16/KERMIT (2 bytes, little-endian)
   */

  uint8_t frame[17];
  size_t pos = 0;

  // Size = bytes after ctrl0, excluding CRC = 14
  uint8_t payload_size = 14;

  // Control Byte 0: order=00, mode=2W, size=14
  frame[pos++] = (0x00 << 6) | IOHC_MODE_2W | (payload_size & 0x1F);

  // Control Byte 1: no special flags, protocol version 0
  frame[pos++] = 0x00;

  // Destination NodeID (3 bytes, big-endian)
  frame[pos++] = (target_address >> 16) & 0xFF;
  frame[pos++] = (target_address >> 8) & 0xFF;
  frame[pos++] = target_address & 0xFF;

  // Source NodeID (3 bytes, big-endian)
  frame[pos++] = (this->source_address_ >> 16) & 0xFF;
  frame[pos++] = (this->source_address_ >> 8) & 0xFF;
  frame[pos++] = this->source_address_ & 0xFF;

  // Command ID
  frame[pos++] = command;

  // Originator: 0x01 = user
  frame[pos++] = 0x01;

  // ACEI: 0x00 = no flags
  frame[pos++] = 0x00;

  // Main Parameter (2 bytes, big-endian)
  frame[pos++] = (main_param >> 8) & 0xFF;
  frame[pos++] = main_param & 0xFF;

  // Functional Parameter 1
  frame[pos++] = 0x00;

  // Functional Parameter 2
  frame[pos++] = 0x00;

  // CRC-16/KERMIT (little-endian)
  uint16_t crc = compute_crc(frame, pos);
  frame[pos++] = crc & 0xFF;
  frame[pos++] = (crc >> 8) & 0xFF;

  ESP_LOGD(TAG, "Sending cover command: target=0x%06X cmd=0x%02X param=0x%04X",
           static_cast<unsigned int>(target_address), command, main_param);

  return this->send_frame(frame, pos);
}

}  // namespace iown_homecontrol
}  // namespace esphome
