/**
 * @file iown_homecontrol.cpp
 * @brief ESPHome component for io-homecontrol protocol - implementation
 */

#include "iown_homecontrol.h"
#include "iown_cover.h"

#if defined(USE_ESP32)
#include <mbedtls/aes.h>
#endif

namespace esphome {
namespace iown_homecontrol {

static const char *const TAG = "iown_homecontrol";

volatile bool IOWNHomeControlComponent::packet_flag_ = false;

// Defined out of line and kept trivial so the IRAM placement stays linkable;
// see the note in the header.
void IRAM_ATTR IOWNHomeControlComponent::packet_isr_() { packet_flag_ = true; }

bool IOWNHomeControlComponent::take_packet_flag_() {
  if (!packet_flag_) {
    return false;
  }
  packet_flag_ = false;
  return true;
}

void IOWNHomeControlComponent::clear_packet_flag_() { packet_flag_ = false; }

void IOWNHomeControlComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up io-homecontrol...");

  // Initialize SPI with custom pins if configured
  if (this->sck_pin_ >= 0 && this->miso_pin_ >= 0 && this->mosi_pin_ >= 0) {
    SPI.begin(this->sck_pin_, this->miso_pin_, this->mosi_pin_);
    ESP_LOGD(TAG, "SPI initialized: SCK=%d, MISO=%d, MOSI=%d", this->sck_pin_, this->miso_pin_,
             this->mosi_pin_);
  }

  this->radio_module_ = new Module(this->cs_pin_, this->dio0_pin_, this->rst_pin_, this->dio1_pin_);

  int16_t state = RADIOLIB_ERR_UNKNOWN;

  switch (this->radio_type_) {
    case RADIO_SX1276: {
      this->sx1276_ = new SX1276(this->radio_module_);
      state = this->sx1276_->beginFSK();
      this->phy_ = this->sx1276_;
      ESP_LOGD(TAG, "Radio type: SX1276");
      break;
    }
    case RADIO_SX1262: {
      this->sx1262_ = new SX1262(this->radio_module_);
      state = this->sx1262_->beginFSK();
      this->phy_ = this->sx1262_;
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

  state = this->configure_phy_layer_();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Physical layer configuration failed: %d", state);
    this->mark_failed();
    return;
  }

  state = this->configure_packet_format_();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Packet format configuration failed: %d", state);
    this->mark_failed();
    return;
  }

  this->restore_rolling_code_();

  // Set up interrupt-driven receive
  this->phy_->setPacketReceivedAction(packet_isr_);
  clear_packet_flag_();

  state = this->phy_->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "Failed to start receive mode: %d", state);
  }

  ESP_LOGI(TAG, "io-homecontrol initialized on %.2f MHz", this->frequency_);
}

void IOWNHomeControlComponent::loop() { this->receive_frame_(); }

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
  ESP_LOGCONFIG(TAG, "  Radio Type: %s", this->radio_type_ == RADIO_SX1276 ? "SX1276" : "SX1262");
  ESP_LOGCONFIG(TAG, "  Source Address: 0x%06X", static_cast<unsigned int>(this->source_address_));
  ESP_LOGCONFIG(TAG, "  Encryption: %s", this->encryption_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  ACEI: 0x%02X", this->acei_);
  ESP_LOGCONFIG(TAG, "  Originator: 0x%02X", this->originator_);
  ESP_LOGCONFIG(TAG, "  Position feedback: %s", this->position_feedback_ ? "enabled" : "disabled");
  if (this->encryption_enabled_) {
    ESP_LOGCONFIG(TAG, "  Rolling code: %u", static_cast<unsigned>(this->rolling_code_));
  }

  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed");
  }
}

int16_t IOWNHomeControlComponent::configure_phy_layer_() {
  int16_t state;

  state = this->phy_->setFrequency(this->frequency_);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set frequency: %d", state);
    return state;
  }

  // RadioLib expresses the FSK data rate in kbps and the deviation in kHz.
  // Passing 38400/19200 here is out of range and fails the whole setup.
  DataRate_t dr;
  dr.fsk.bitRate = IOHC_BITRATE_KBPS;
  dr.fsk.freqDev = IOHC_DEVIATION_KHZ;
  state = this->phy_->setDataRate(dr);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set data rate: %d", state);
    return state;
  }

  state = this->phy_->setDataShaping(RADIOLIB_SHAPING_NONE);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set data shaping: %d", state);
    return state;
  }

  state = this->phy_->setEncoding(RADIOLIB_ENCODING_NRZ);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set encoding: %d", state);
    return state;
  }

  uint8_t sync_word[IOHC_SYNC_WORD_LEN];
  memcpy(sync_word, IOHC_SYNC_WORD, IOHC_SYNC_WORD_LEN);
  state = this->phy_->setSyncWord(sync_word, IOHC_SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set sync word: %d", state);
    return state;
  }

  // RadioLib takes the FSK preamble length in bits, so pass 512 through as-is.
  state = this->phy_->setPreambleLength(IOHC_PREAMBLE_BITS);
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

int16_t IOWNHomeControlComponent::configure_packet_format_() {
  // io-homecontrol frames carry their own length (Control Byte 0) and their own
  // CRC. RadioLib's FSK defaults would prepend a length byte and append a
  // second CRC, corrupting every frame in both directions.
  //
  // Receive stays at the maximum frame size. In fixed-length mode the radio
  // only signals a packet once it has collected that many bytes, so a shorter
  // frame completes after a few more bytes of the next preamble or of noise;
  // parse_frame_() takes the real length from Control Byte 0 and ignores the
  // rest. Variable-length mode cannot work here - RadioLib would read Control
  // Byte 0 as a byte count, and it is not one. See docs/RADIO-SETUP.md.
  int16_t state = RADIOLIB_ERR_NONE;

  if (this->sx1276_ != nullptr) {
    state = this->sx1276_->setCRC(false);
    if (state != RADIOLIB_ERR_NONE) {
      ESP_LOGE(TAG, "Failed to disable radio CRC: %d", state);
      return state;
    }
    state = this->sx1276_->fixedPacketLengthMode(IOHC_MAX_FRAME_SIZE);
  } else if (this->sx1262_ != nullptr) {
    // SX126x takes the CRC length in bytes; 0 disables it.
    state = this->sx1262_->setCRC(0);
    if (state != RADIOLIB_ERR_NONE) {
      ESP_LOGE(TAG, "Failed to disable radio CRC: %d", state);
      return state;
    }
    state = this->sx1262_->fixedPacketLengthMode(IOHC_MAX_FRAME_SIZE);
  }

  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Failed to set fixed packet length mode: %d", state);
  }

  return state;
}

int16_t IOWNHomeControlComponent::set_packet_length_(uint8_t len) {
  if (this->sx1276_ != nullptr) {
    return this->sx1276_->fixedPacketLengthMode(len);
  }
  if (this->sx1262_ != nullptr) {
    return this->sx1262_->fixedPacketLengthMode(len);
  }
  return RADIOLIB_ERR_NONE;
}

uint16_t IOWNHomeControlComponent::compute_crc(const uint8_t *data, size_t len) {
  // The implementation lives in iohc_protocol.h, which has no dependencies and
  // is therefore buildable by test/test_esphome_crypto on the host.
  return iohc_compute_crc(data, len);
}

uint16_t IOWNHomeControlComponent::main_param_from_percent_closed(uint8_t percent_closed) {
  if (percent_closed >= 100) {
    return IOHC_PARAM_PERCENT_MAX;
  }
  return static_cast<uint16_t>((static_cast<uint32_t>(percent_closed) * IOHC_PARAM_PERCENT_MAX) /
                               100u);
}

uint8_t IOWNHomeControlComponent::percent_closed_from_main_param(uint16_t main_param) {
  if (main_param > IOHC_PARAM_PERCENT_MAX) {
    return 0xFF;  // Not a percentage: a function code such as 0xD200 (stop)
  }
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(main_param) * 100u + IOHC_PARAM_PERCENT_MAX / 2u) /
      IOHC_PARAM_PERCENT_MAX);
}

void IOWNHomeControlComponent::set_system_key(const std::vector<uint8_t> &key) {
  if (key.size() != sizeof(this->system_key_)) {
    ESP_LOGE(TAG, "System key must be %u bytes, got %u", static_cast<unsigned>(sizeof(this->system_key_)),
             static_cast<unsigned>(key.size()));
    return;
  }
  memcpy(this->system_key_, key.data(), sizeof(this->system_key_));
  this->system_key_set_ = true;
}

void IOWNHomeControlComponent::restore_rolling_code_() {
  // Key the preference on the source address so two hubs on one device do not
  // share a counter.
  const uint32_t hash = fnv1_hash("iown_homecontrol_rolling_code_" +
                                  std::to_string(this->source_address_));
  this->rolling_code_pref_ = global_preferences->make_preference<uint16_t>(hash);

  uint16_t stored = 0;
  if (this->rolling_code_pref_.load(&stored)) {
    ESP_LOGD(TAG, "Restored rolling code: %u", static_cast<unsigned>(stored));
  } else {
    ESP_LOGD(TAG, "No stored rolling code, starting at 0");
    stored = 0;
  }

  this->rolling_code_ = stored;
  this->rolling_code_reserved_until_ = static_cast<uint16_t>(stored + ROLLING_CODE_RESERVE_BLOCK);
  this->rolling_code_pref_.save(&this->rolling_code_reserved_until_);
}

uint16_t IOWNHomeControlComponent::consume_rolling_code_() {
  const uint16_t code = this->rolling_code_;
  this->rolling_code_ = static_cast<uint16_t>(this->rolling_code_ + 1);

  // Only write when the reserved block runs out; writing on every command
  // would wear out the NVS partition. The subtraction is modular, so it stays
  // correct across the 16-bit wrap.
  const uint16_t remaining =
      static_cast<uint16_t>(this->rolling_code_reserved_until_ - this->rolling_code_);
  if (remaining == 0 || remaining > ROLLING_CODE_RESERVE_BLOCK) {
    this->rolling_code_reserved_until_ =
        static_cast<uint16_t>(this->rolling_code_ + ROLLING_CODE_RESERVE_BLOCK);
    this->rolling_code_pref_.save(&this->rolling_code_reserved_until_);
  }

  return code;
}

bool IOWNHomeControlComponent::compute_hmac_(const uint8_t *frame_data, size_t data_len,
                                             const uint8_t rolling_code[2], uint8_t hmac_out[6]) {
#if defined(USE_ESP32)
  // The initial value is built in iohc_protocol.h so the host test can check it
  // against src/protocol/ and against the captures in docs/. Only the AES block
  // itself needs mbedTLS, which is why it stays here.
  uint8_t iv[IOHC_IV_SIZE];
  iohc_build_iv_1w(frame_data, data_len, rolling_code, iv);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, this->system_key_, 128) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }
  uint8_t encrypted[16];
  if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, iv, encrypted) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }
  mbedtls_aes_free(&aes);

  // Truncate to 6 bytes
  memcpy(hmac_out, encrypted, 6);

  // Do not leave key-derived material on the stack.
  memset(encrypted, 0, sizeof(encrypted));
  memset(iv, 0, sizeof(iv));
  return true;
#else
  ESP_LOGE(TAG, "Encryption requires ESP32 (mbedTLS not available)");
  (void) frame_data;
  (void) data_len;
  (void) rolling_code;
  (void) hmac_out;
  return false;
#endif
}

void IOWNHomeControlComponent::receive_frame_() {
  if (this->phy_ == nullptr || this->is_failed()) {
    return;
  }

  if (!take_packet_flag_()) {
    return;
  }

  const size_t len = this->phy_->getPacketLength();
  if (len == 0 || len > IOHC_MAX_FRAME_SIZE) {
    this->phy_->startReceive();
    return;
  }

  uint8_t data[IOHC_MAX_FRAME_SIZE];
  const int16_t state = this->phy_->readData(data, len);

  // Sample the link metric before handing the radio back to receive mode.
  const int16_t rssi = static_cast<int16_t>(this->phy_->getRSSI());

  this->phy_->startReceive();

  if (state == RADIOLIB_ERR_NONE) {
    this->parse_frame_(data, len, rssi);
  } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
    ESP_LOGW(TAG, "Receive error: %d", state);
  }
}

void IOWNHomeControlComponent::parse_frame_(const uint8_t *data, size_t len, int16_t rssi) {
  if (len < IOHC_MIN_FRAME_SIZE) {
    ESP_LOGW(TAG, "Frame too short: %u bytes", static_cast<unsigned>(len));
    return;
  }

  const uint8_t ctrl0 = data[0];
  const uint8_t ctrl1 = data[1];
  const uint8_t order = (ctrl0 >> 6) & 0x03;

  // Control Byte 0 bit 5 is `isOneWay`: 1 means 1W, 0 means 2W.
  const bool is_1w = (ctrl0 & IOHC_CTRL0_ONE_WAY) != 0;

  // `Size` excludes Control Byte 0 and the CRC, so the frame is Size + 3 long.
  const size_t frame_len = static_cast<size_t>(ctrl0 & IOHC_CTRL0_SIZE_MASK) + IOHC_SIZE_BIAS;

  if (frame_len < IOHC_MIN_FRAME_SIZE || frame_len > len) {
    ESP_LOGW(TAG, "Declared frame length %u does not fit in %u received bytes",
             static_cast<unsigned>(frame_len), static_cast<unsigned>(len));
    return;
  }

  // Fixed-length receive mode delivers trailing bytes past the frame, so every
  // field below is taken relative to frame_len, never to the received count.
  const uint32_t dest_addr =
      (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 8) | data[4];
  const uint32_t src_addr =
      (static_cast<uint32_t>(data[5]) << 16) | (static_cast<uint32_t>(data[6]) << 8) | data[7];
  const uint8_t cmd = data[8];

  // CRC-16/KERMIT over everything but the last two bytes, LSB transmitted first.
  const uint16_t received_crc =
      static_cast<uint16_t>(data[frame_len - 2] | (static_cast<uint16_t>(data[frame_len - 1]) << 8));
  const uint16_t calculated_crc = compute_crc(data, frame_len - 2);

  if (received_crc != calculated_crc) {
    this->crc_errors_++;
    ESP_LOGW(TAG, "CRC mismatch: received=0x%04X calculated=0x%04X", received_crc, calculated_crc);
    return;
  }

  this->frames_received_++;
  this->last_rssi_ = rssi;

  ESP_LOGI(TAG, "Frame: mode=%s order=%d src=0x%06X dst=0x%06X cmd=0x%02X len=%u rssi=%d",
           is_1w ? "1W" : "2W", order, static_cast<unsigned int>(src_addr),
           static_cast<unsigned int>(dest_addr), cmd, static_cast<unsigned>(frame_len), rssi);

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  // Only build the hex dump when it can actually be logged.
  {
    std::string hex_str;
    hex_str.reserve(frame_len * 3);
    for (size_t i = 0; i < frame_len; i++) {
      char buf[4];
      snprintf(buf, sizeof(buf), "%02X ", data[i]);
      hex_str += buf;
    }
    ESP_LOGD(TAG, "Raw: %s", hex_str.c_str());
  }
#endif

  ReceivedFrame frame{};
  frame.ctrl0 = ctrl0;
  frame.ctrl1 = ctrl1;
  frame.dest_address = dest_addr;
  frame.src_address = src_addr;
  frame.command = cmd;
  frame.payload = &data[9];
  // Payload runs from the byte after the command up to the CRC. For an
  // authenticated 1W frame this still includes the sequence number and MAC.
  frame.payload_len = frame_len - 9 - 2;
  frame.is_1w = is_1w;
  frame.rssi = rssi;

  if (cmd == IOHC_CMD_EXECUTE && frame.payload_len >= IOHC_EXEC_PAYLOAD_SIZE) {
    // Payload layout, counted from the command byte at index 8:
    //   originator | ACEI | main parameter (2, MSB first) | FP1 | FP2
    // Reading the main parameter from bytes 9-10 picks up the originator and
    // the ACEI instead.
    const uint8_t *exec = &data[8];
    const uint8_t originator = exec[IOHC_EXEC_OFFSET_ORIGINATOR];
    const uint8_t acei = exec[IOHC_EXEC_OFFSET_ACEI];
    const uint16_t main_param = static_cast<uint16_t>(
        (static_cast<uint16_t>(exec[IOHC_EXEC_OFFSET_MAIN_PARAM]) << 8) |
        exec[IOHC_EXEC_OFFSET_MAIN_PARAM + 1]);

    const char *action = "position";
    if (main_param == IOHC_PARAM_OPEN) {
      action = "OPEN";
    } else if (main_param == IOHC_PARAM_CLOSE) {
      action = "CLOSE";
    } else if (main_param == IOHC_PARAM_STOP) {
      action = "STOP";
    }

    ESP_LOGI(TAG, "Execute: %s (originator=0x%02X acei=0x%02X main=0x%04X)", action, originator,
             acei, main_param);

    this->dispatch_to_covers_(frame);
  }
}

void IOWNHomeControlComponent::dispatch_to_covers_(const ReceivedFrame &frame) {
  if (!this->position_feedback_) {
    return;
  }

  if (frame.payload == nullptr || frame.payload_len < IOHC_EXEC_PAYLOAD_SIZE) {
    return;
  }

  // frame.payload starts at the byte after the command, so the offsets below
  // are one less than the IOHC_EXEC_OFFSET_* values, which count from the
  // command byte itself.
  const uint16_t main_param = static_cast<uint16_t>(
      (static_cast<uint16_t>(frame.payload[IOHC_EXEC_OFFSET_MAIN_PARAM - 1]) << 8) |
      frame.payload[IOHC_EXEC_OFFSET_MAIN_PARAM]);
  const uint8_t percent_closed = percent_closed_from_main_param(main_param);
  if (percent_closed == 0xFF) {
    return;  // Not a position report
  }

  for (IOWNCover *cover : this->covers_) {
    if (cover != nullptr) {
      cover->on_position_report(frame.src_address, percent_closed);
    }
  }
}

bool IOWNHomeControlComponent::send_frame(const uint8_t *data, size_t len) {
  if (this->phy_ == nullptr) {
    ESP_LOGE(TAG, "Radio not initialized");
    return false;
  }

  if (data == nullptr || len == 0 || len > IOHC_MAX_FRAME_SIZE) {
    ESP_LOGE(TAG, "Refusing to send %u bytes", static_cast<unsigned>(len));
    return false;
  }

  // Stop the interrupt from firing while we own the radio, and drop any packet
  // that arrived just before: its data is gone once we transmit.
  this->phy_->clearPacketReceivedAction();
  clear_packet_flag_();

  // In fixed-length mode the radio sends exactly the programmed number of
  // bytes, so narrow it to this frame and widen it again for reception.
  int16_t state = this->set_packet_length_(static_cast<uint8_t>(len));
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "Could not set packet length: %d", state);
  }

  state = this->phy_->transmit(const_cast<uint8_t *>(data), len);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Transmit failed: %d", state);
  }

  this->set_packet_length_(IOHC_MAX_FRAME_SIZE);

  // Restore receive mode
  this->phy_->setPacketReceivedAction(packet_isr_);
  clear_packet_flag_();
  this->phy_->startReceive();

  return state == RADIOLIB_ERR_NONE;
}

bool IOWNHomeControlComponent::send_cover_command(uint32_t target_address, uint8_t command,
                                                  uint16_t main_param, uint8_t fp1, uint8_t fp2) {
  if (this->is_failed()) {
    return false;
  }

  if (this->encryption_enabled_ && !this->system_key_set_) {
    ESP_LOGE(TAG, "Encryption enabled but no system key configured");
    return false;
  }

  /*
   * 1W authenticated frame (25 bytes) / 2W plain frame (17 bytes):
   *
   * Byte 0:     Control Byte 0 (order, isOneWay, size)
   * Byte 1:     Control Byte 1
   * Byte 2-4:   Destination NodeID (3 bytes, big-endian)
   * Byte 5-7:   Source NodeID (3 bytes, big-endian)
   * Byte 8:     Command ID
   * Byte 9:     Command Originator
   * Byte 10:    ACEI flags
   * Byte 11-12: Main Parameter (2 bytes, big-endian)
   * Byte 13:    Functional Parameter 1
   * Byte 14:    Functional Parameter 2
   * 1W only:
   * Byte 15-16: Sequence number (2 bytes, LSB first)
   * Byte 17-22: MAC (6 bytes)
   * Last two:   CRC-16/KERMIT (LSB first)
   */

  uint8_t frame[IOHC_MAX_FRAME_SIZE];
  size_t pos = 0;

  const bool authenticated = this->encryption_enabled_;

  // header(9) + execute payload(6) + [seq(2) + mac(6)] + crc(2)
  const size_t total_len = 9 + IOHC_EXEC_PAYLOAD_SIZE + (authenticated ? 8u : 0u) + 2u;

  // Size field excludes Control Byte 0 and the CRC.
  const uint8_t size_field = static_cast<uint8_t>((total_len - IOHC_SIZE_BIAS) & IOHC_CTRL0_SIZE_MASK);

  frame[pos++] = static_cast<uint8_t>((authenticated ? IOHC_MODE_1W : IOHC_MODE_2W) | size_field);
  frame[pos++] = 0x00;  // Control Byte 1

  frame[pos++] = (target_address >> 16) & 0xFF;
  frame[pos++] = (target_address >> 8) & 0xFF;
  frame[pos++] = target_address & 0xFF;

  frame[pos++] = (this->source_address_ >> 16) & 0xFF;
  frame[pos++] = (this->source_address_ >> 8) & 0xFF;
  frame[pos++] = this->source_address_ & 0xFF;

  frame[pos++] = command;
  frame[pos++] = this->originator_;

  // Bit 0 of the ACEI byte must be set or actuators discard the frame.
  frame[pos++] = static_cast<uint8_t>(this->acei_ | IOHC_ACEI_VALID_MASK);

  frame[pos++] = (main_param >> 8) & 0xFF;
  frame[pos++] = main_param & 0xFF;
  frame[pos++] = fp1;
  frame[pos++] = fp2;

  if (authenticated) {
    // The MAC covers the command ID and its parameters.
    const uint8_t *hmac_data = &frame[8];
    const size_t hmac_data_len = 1 + IOHC_EXEC_PAYLOAD_SIZE;

    const uint16_t sequence = this->consume_rolling_code_();
    uint8_t rc[2];
    rc[0] = sequence & 0xFF;  // LSB first on the wire
    rc[1] = (sequence >> 8) & 0xFF;

    uint8_t hmac[6];
    if (!this->compute_hmac_(hmac_data, hmac_data_len, rc, hmac)) {
      ESP_LOGE(TAG, "MAC computation failed");
      return false;
    }

    frame[pos++] = rc[0];
    frame[pos++] = rc[1];

    memcpy(&frame[pos], hmac, sizeof(hmac));
    pos += sizeof(hmac);

    ESP_LOGD(TAG, "Sending 1W command: target=0x%06X cmd=0x%02X main=0x%04X seq=%u",
             static_cast<unsigned int>(target_address), command, main_param,
             static_cast<unsigned>(sequence));
  } else {
    ESP_LOGD(TAG, "Sending 2W command: target=0x%06X cmd=0x%02X main=0x%04X",
             static_cast<unsigned int>(target_address), command, main_param);
  }

  const uint16_t crc = compute_crc(frame, pos);
  frame[pos++] = crc & 0xFF;
  frame[pos++] = (crc >> 8) & 0xFF;

  // The computed layout and the announced size must agree, or receivers will
  // parse a different frame than the one we built.
  if (pos != total_len) {
    ESP_LOGE(TAG, "Internal error: built %u bytes but announced %u", static_cast<unsigned>(pos),
             static_cast<unsigned>(total_len));
    return false;
  }

  return this->send_frame(frame, pos);
}

}  // namespace iown_homecontrol
}  // namespace esphome
