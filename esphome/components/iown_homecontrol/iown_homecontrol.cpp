/**
 * @file iown_homecontrol.cpp
 * @brief ESPHome component for io-homecontrol protocol - implementation
 */

#include "iown_homecontrol.h"
#include "iown_cover.h"

#if defined(USE_ESP32)
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

void IOWNHomeControlComponent::power_up_frontend_() {
  // Order matters. On boards that gate the radio's supply behind a GPIO, the
  // rail has to be up before the reset pulse - a reset into an unpowered
  // module leaves the SX126x in an undefined state that only looks like a
  // wiring fault. The front end's LDO comes next, because its control pins do
  // nothing until it is fed.
  if (this->vext_pin_ >= 0) {
    pinMode(this->vext_pin_, OUTPUT);
    digitalWrite(this->vext_pin_, this->vext_active_high_ ? HIGH : LOW);
    ESP_LOGD(TAG, "VEXT rail on (GPIO%d, active %s)", this->vext_pin_,
             this->vext_active_high_ ? "high" : "low");
  }

  if (this->fem_power_pin_ >= 0) {
    pinMode(this->fem_power_pin_, OUTPUT);
    digitalWrite(this->fem_power_pin_, HIGH);
  }
  if (this->fem_enable_pin_ >= 0) {
    pinMode(this->fem_enable_pin_, OUTPUT);
    digitalWrite(this->fem_enable_pin_, HIGH);
  }
  // fem_tx_pin_ is deliberately not touched here: RadioLib owns it once
  // setRfSwitchPins() has been told about it, and driving it from both sides
  // would fight over the TX/RX path.

  if (this->vext_pin_ >= 0 || this->fem_power_pin_ >= 0) {
    // Let the rails settle before the first SPI transaction.
    delay(10);
  }
}

void IOWNHomeControlComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up io-homecontrol...");

  this->power_up_frontend_();

  // Initialize SPI with custom pins if configured
  if (this->sck_pin_ >= 0 && this->miso_pin_ >= 0 && this->mosi_pin_ >= 0) {
    SPI.begin(this->sck_pin_, this->miso_pin_, this->mosi_pin_);
    ESP_LOGD(TAG, "SPI initialized: SCK=%d, MISO=%d, MOSI=%d", this->sck_pin_, this->miso_pin_,
             this->mosi_pin_);
  }

  // RadioLib's Module constructor is Module(cs, irq, rst, gpio), but the two
  // families disagree about which physical pin plays which role:
  //
  //   SX127x: irq = DIO0 (PayloadReady), gpio = DIO1
  //   SX126x: irq = DIO1 (the only interrupt line broken out), gpio = BUSY
  //
  // This used to pass dio0_pin_ as irq for both. An SX1262 therefore had to be
  // configured with its DIO1 in `dio0_pin` and its BUSY in `dio1_pin` to work
  // at all - which is what the example config quietly told people to do. Two
  // errors cancelling is not the same as being right: anyone who filled the
  // fields in honestly got a radio whose interrupt never fired.
  if (this->radio_type_ == RADIO_SX1262) {
    this->radio_module_ = new Module(this->cs_pin_, this->dio1_pin_, this->rst_pin_, this->busy_pin_);
  } else {
    this->radio_module_ = new Module(this->cs_pin_, this->dio0_pin_, this->rst_pin_, this->dio1_pin_);
  }

  // An external PA needs its TX/RX select line to follow the radio's own mode
  // changes. RadioLib drives txEn high for transmit and low for receive, which
  // is exactly the GC1109's CPS semantics (high = PA, low = receive bypass).
  // This has to be registered on the Module before begin(), so the switch is
  // already in a defined state for the calibration begin() performs.
  if (this->fem_tx_pin_ >= 0) {
    this->radio_module_->setRfSwitchPins(RADIOLIB_NC, this->fem_tx_pin_);
    ESP_LOGD(TAG, "RF switch: TX enable on GPIO%d", this->fem_tx_pin_);
  }

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
      // The TCXO supply voltage is only settable through begin(): SX126x
      // latches it and applies it from config(), which runs here. Everything
      // else in this call is a placeholder - configure_phy_layer_() programs
      // the real frequency, bit rate, deviation and preamble immediately
      // afterwards - but the defaults have to be spelled out to reach the
      // seventh parameter.
      state = this->sx1262_->beginFSK(434.0f,   // frequency, overwritten below
                                      4.8f,     // bit rate, overwritten below
                                      5.0f,     // deviation, overwritten below
                                      156.2f,   // RX bandwidth, overwritten below
                                      10,       // output power, overwritten below
                                      16,       // preamble, overwritten below
                                      this->tcxo_voltage_);
      this->phy_ = this->sx1262_;
      ESP_LOGD(TAG, "Radio type: SX1262 (TCXO %.1f V)", this->tcxo_voltage_);
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
  // Name the pins by what they are on this radio, so a mis-wired board is
  // visible in the log rather than only in its silence.
  if (this->radio_type_ == RADIO_SX1262) {
    ESP_LOGCONFIG(TAG, "  DIO1 Pin (IRQ): %d", this->dio1_pin_);
    ESP_LOGCONFIG(TAG, "  BUSY Pin: %d", this->busy_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  DIO0 Pin (IRQ): %d", this->dio0_pin_);
    ESP_LOGCONFIG(TAG, "  DIO1 Pin: %d", this->dio1_pin_);
  }
  if (this->sck_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  SCK Pin: %d", this->sck_pin_);
    ESP_LOGCONFIG(TAG, "  MOSI Pin: %d", this->mosi_pin_);
    ESP_LOGCONFIG(TAG, "  MISO Pin: %d", this->miso_pin_);
  }
  ESP_LOGCONFIG(TAG, "  Frequency: %.2f MHz", this->frequency_);
  ESP_LOGCONFIG(TAG, "  Radio Type: %s", this->radio_type_ == RADIO_SX1276 ? "SX1276" : "SX1262");
  if (this->radio_type_ == RADIO_SX1262) {
    if (this->tcxo_voltage_ > 0.0f) {
      ESP_LOGCONFIG(TAG, "  TCXO: %.1f V on DIO3", this->tcxo_voltage_);
    } else {
      ESP_LOGCONFIG(TAG, "  TCXO: disabled (crystal)");
    }
  }
  if (this->vext_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  VEXT Pin: %d (active %s)", this->vext_pin_,
                  this->vext_active_high_ ? "high" : "low");
  }
  if (this->fem_tx_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  RF front end: LDO %d, enable %d, TX select %d", this->fem_power_pin_,
                  this->fem_enable_pin_, this->fem_tx_pin_);
  }
  ESP_LOGCONFIG(TAG, "  Source Address: 0x%06X", static_cast<unsigned int>(this->source_address_));
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->two_way_ ? "2W (challenge-response)" : "1W (rolling code)");
  ESP_LOGCONFIG(TAG, "  Encryption: %s", this->encryption_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  ACEI: 0x%02X", this->acei_);
  ESP_LOGCONFIG(TAG, "  Originator: 0x%02X", this->originator_);
  ESP_LOGCONFIG(TAG, "  Position feedback: %s", this->position_feedback_ ? "enabled" : "disabled");
  if (this->position_feedback_ && !this->system_key_set_) {
    ESP_LOGW(TAG, "  position_feedback needs system_key: without it no report can be");
    ESP_LOGW(TAG, "  authenticated, so every one of them will be ignored");
  }
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
    state = this->sx1276_->fixedPacketLengthMode(IOHC_RX_CAPTURE_SIZE);
  } else if (this->sx1262_ != nullptr) {
    // SX126x takes the CRC length in bytes; 0 disables it.
    state = this->sx1262_->setCRC(0);
    if (state != RADIOLIB_ERR_NONE) {
      ESP_LOGE(TAG, "Failed to disable radio CRC: %d", state);
      return state;
    }
    state = this->sx1262_->fixedPacketLengthMode(IOHC_RX_CAPTURE_SIZE);
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
  return iohome::crypto::compute_crc16(data, len);
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
  // The authentication manager needs the key before any handshake starts.
  this->auth_.begin(this->system_key_);
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

  // Do NOT reserve or persist a block here. consume_rolling_code_() reserves and
  // writes a block before it hands out the first code (its remaining wraps to
  // 0xFFFF on the first call), so the no-reuse guarantee holds. Reserving eagerly
  // on every boot burned a full block even when the run transmitted nothing, so a
  // run of no-op reboots (brownouts, OTA/config/WiFi restarts) could skip the
  // transmit counter past the actuator's bounded rolling-code window and lock the
  // hub out with no attacker involved. This mirrors the library-side V72 fix in
  // src/IoHomeControl.cpp begin().
  this->rolling_code_ = stored;
  this->rolling_code_reserved_until_ = stored;  // no live reservation until first use
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
  // This was sixty lines of initial-value construction and mbedTLS calls,
  // duplicating src/protocol/iohome_crypto.cpp. The protocol layer is compiled
  // into the component now, so there is nothing left to duplicate.
  return iohome::crypto::create_1w_hmac(frame_data, data_len, rolling_code, this->system_key_,
                                        hmac_out);
}

// ---------------------------------------------------------------------------
// 2W challenge-response
//
// A 1W frame proves itself with a rolling code, which a receiver can only
// check if it already knows where the counter stands. 2W instead binds each
// MAC to a nonce the *receiver* chose moments earlier, so a recorded frame is
// worthless the instant the session ends.
//
// The exchange is: we send command 0x3C carrying a random challenge, the peer
// answers 0x3D with a MAC over it, and from then until the session times out
// every command we send is MAC'd against that same challenge.
// ---------------------------------------------------------------------------

bool IOWNHomeControlComponent::frame_is_fresh_(const iohome::frame::IoFrame &frame) {
  if (frame.is_1w_mode) {
    return this->replay_guard_.accept(frame.src_node, iohome::frame::get_rolling_code(&frame));
  }

  // 2W frames carry no sequence number, so the rolling-code check has nothing
  // to work with - and the component used to simply skip the check for them.
  // A 2W position report was therefore authenticated but never fresh: record
  // one and re-transmit it, and it was applied again every time, for as long
  // as the session lasted. Freshness here comes from the session challenge,
  // which signs several frames, so what is left to catch is a MAC this node
  // produced recently - a protocol repeat to act on once, or a replay.
  return this->replay_guard_.accept_mac(frame.src_node, frame.hmac);
}

bool IOWNHomeControlComponent::send_protocol_frame_(const iohome::frame::IoFrame *frame) {
  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(frame, buffer, sizeof(buffer));
  if (len == 0) {
    ESP_LOGE(TAG, "Frame did not serialize");
    return false;
  }
  return this->send_frame(buffer, len);
}

bool IOWNHomeControlComponent::is_2w_authenticated() {
  return this->two_way_ && this->auth_.is_authenticated(millis());
}

bool IOWNHomeControlComponent::ensure_2w_session_(uint32_t target_address) {
  const uint32_t now = millis();

  uint8_t dest[iohome::NODE_ID_SIZE];
  uint8_t src[iohome::NODE_ID_SIZE];
  address_to_node(target_address, dest);
  address_to_node(this->source_address_, src);

  // The session must belong to THIS actuator. A session negotiated with a
  // different cover cannot sign a frame to this one - its challenge nonce only
  // signs frames between the two nodes that negotiated it - so reusing it would
  // send a frame the target silently rejects while we report success. When the
  // held session is for another peer, fall through and challenge this target.
  if (this->auth_.is_authenticated_with(dest, now)) {
    return true;
  }

  if (this->auth_.get_state(now) == iohome::mode2w::ChallengeState::CHALLENGE_SENT) {
    // A request is already in flight. Saying so beats sending a second one and
    // invalidating the challenge the peer is answering.
    ESP_LOGD(TAG, "2W handshake in progress, command not sent");
    return false;
  }

  iohome::frame::IoFrame request;
  if (!this->auth_.create_challenge_request(&request, dest, src, now)) {
    // The only way this fails with valid arguments is no secure random source,
    // and a predictable challenge is worse than no session at all.
    ESP_LOGE(TAG, "Could not create a 2W challenge - no secure random source?");
    return false;
  }

  ESP_LOGI(TAG, "2W: challenging 0x%06X", static_cast<unsigned int>(target_address));
  this->send_protocol_frame_(&request);
  return false;
}

bool IOWNHomeControlComponent::send_2w_command_(uint32_t target_address, uint16_t main_param,
                                                uint8_t fp1, uint8_t fp2) {
  if (!this->ensure_2w_session_(target_address)) {
    return false;
  }

  uint8_t dest[iohome::NODE_ID_SIZE];
  uint8_t src[iohome::NODE_ID_SIZE];
  address_to_node(target_address, dest);
  address_to_node(this->source_address_, src);

  iohome::frame::IoFrame frame;
  iohome::frame::init_frame(&frame, false);  // 2W
  iohome::frame::set_destination(&frame, dest);
  iohome::frame::set_source(&frame, src);

  if (!iohome::frame::set_execute_command(
          &frame, main_param, static_cast<iohome::Originator>(this->originator_),
          static_cast<uint8_t>(this->acei_ | IOHC_ACEI_VALID_MASK), fp1, fp2)) {
    ESP_LOGE(TAG, "Rejected execute payload (ACEI 0x%02X?)", this->acei_);
    return false;
  }

  // The MAC is bound to the challenge the peer chose, which is what makes a
  // recorded 2W frame useless after the session ends.
  if (!iohome::frame::finalize_frame(&frame, this->system_key_,
                                     this->auth_.get_current_challenge())) {
    ESP_LOGE(TAG, "Could not sign the 2W frame");
    return false;
  }

  ESP_LOGD(TAG, "Sending 2W command: target=0x%06X main=0x%04X",
           static_cast<unsigned int>(target_address), main_param);
  return this->send_protocol_frame_(&frame);
}

void IOWNHomeControlComponent::handle_challenge_frame_(const iohome::frame::IoFrame *frame) {
  const uint32_t now = millis();

  if (frame->command_id == iohome::CMD_CHALLENGE_RESPONSE) {
    if (this->auth_.verify_challenge_response(frame, now)) {
      ESP_LOGI(TAG, "2W session authenticated");
    } else {
      ESP_LOGW(TAG, "2W challenge response rejected");
      this->mac_errors_++;
    }
    return;
  }

  if (frame->command_id == iohome::CMD_CHALLENGE_REQUEST) {
    // The peer is challenging us. Its nonce travels in the request's own
    // payload - answering with a challenge of ours would prove nothing.
    if (frame->data_len < iohome::HMAC_SIZE) {
      ESP_LOGW(TAG, "Challenge request too short to carry a nonce");
      return;
    }

    // The request is signed against the nonce it carries, and that signature
    // has to hold before we answer. Answering an unverified request meant
    // anyone in radio range could hand this hub a nonce of their choosing and
    // read back the MAC computed over it with the system key - a chosen-input
    // oracle, offered to strangers, on request.
    if (!this->system_key_set_ ||
        !iohome::frame::validate_frame(frame, this->system_key_, frame->data)) {
      ESP_LOGW(TAG, "Unsigned challenge request from 0x%02X%02X%02X - not answering",
               frame->src_node[0], frame->src_node[1], frame->src_node[2]);
      this->mac_errors_++;
      return;
    }

    iohome::frame::IoFrame response;
    if (!this->auth_.create_challenge_response(&response, frame->src_node, frame->dest_node,
                                               frame->data)) {
      ESP_LOGW(TAG, "Could not answer the peer's challenge");
      return;
    }
    ESP_LOGI(TAG, "Answering a peer-initiated 2W challenge");
    this->send_protocol_frame_(&response);
  }
}

void IOWNHomeControlComponent::receive_frame_() {
  if (this->phy_ == nullptr || this->is_failed()) {
    return;
  }

  if (!take_packet_flag_()) {
    return;
  }

  const size_t len = this->phy_->getPacketLength();
  if (len == 0 || len > IOHC_RX_CAPTURE_SIZE) {
    this->phy_->startReceive();
    return;
  }

  uint8_t raw[IOHC_RX_CAPTURE_SIZE];
  const int16_t state = this->phy_->readData(raw, len);

  // Sample the link metric before handing the radio back to receive mode.
  const int16_t rssi = static_cast<int16_t>(this->phy_->getRSSI());

  this->phy_->startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    if (state != RADIOLIB_ERR_RX_TIMEOUT) {
      ESP_LOGW(TAG, "Receive error: %d", state);
    }
    return;
  }

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  // The raw on-air bytes, framing and all, before de-framing. This is what a
  // capture session records; it is logged at VERBOSE so it does not drown the
  // ordinary DEBUG frame log.
  {
    std::string hex;
    hex.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
      char b[4];
      snprintf(b, sizeof(b), "%02X ", raw[i]);
      hex += b;
    }
    ESP_LOGV(TAG, "RAW %u bytes rssi=%d: %s", static_cast<unsigned>(len), rssi, hex.c_str());
  }
#endif

  // The radio delivers the on-air bytes with the UART start/stop framing still
  // on them. De-frame to the actual frame before anything reads it: without
  // this the "control byte" is really a start bit and part of ctrl0, the length
  // runs 10/8 too long, and the CRC never checks out. See iohome_phy_framing.h.
  uint8_t frame[IOHC_MAX_FRAME_SIZE];
  const size_t frame_len = iohome::phy::uart_decode_frame(raw, len, frame, sizeof frame);
  if (frame_len == 0) {
    ESP_LOGV(TAG, "No frame recovered from %u raw bytes", static_cast<unsigned>(len));
    return;
  }

  this->parse_frame_(frame, frame_len, rssi);
}

void IOWNHomeControlComponent::parse_frame_(const uint8_t *data, size_t len, int16_t rssi) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  // The de-framed frame, as the protocol layer sees it. The raw on-air bytes
  // are logged separately at VERBOSE in receive_frame_().
  {
    std::string hex_str;
    hex_str.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
      char buf[4];
      snprintf(buf, sizeof(buf), "%02X ", data[i]);
      hex_str += buf;
    }
    ESP_LOGD(TAG, "RX frame %u bytes rssi=%d: %s", static_cast<unsigned>(len), rssi,
             hex_str.c_str());
  }
#endif

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

  if (frame_len < IOHC_MIN_FRAME_SIZE) {
    // Not the same fault as "longer than what arrived", and reporting both the
    // same way sent this investigation looking at the wrong end of the frame.
    ESP_LOGW(TAG, "Declared frame length %u is below the %u-byte minimum (ctrl0=0x%02X)",
             static_cast<unsigned>(frame_len), static_cast<unsigned>(IOHC_MIN_FRAME_SIZE), ctrl0);
    return;
  }

  if (frame_len > len) {
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
    // A CRC mismatch here now means a genuinely corrupt reception. It used to be
    // the normal case - every captured frame failed this check - because the
    // bytes still carried their UART start/stop framing and this ran over the
    // wrong data. With de-framing in receive_frame_() the CRC validates, so a
    // failure is real: bad reception, not a mystery of the format.
    this->crc_errors_++;
    ESP_LOGW(TAG, "CRC mismatch: received=0x%04X calculated=0x%04X rssi=%d", received_crc,
             calculated_crc, rssi);
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

  // A 1W key transfer is the one frame that cannot be checked against the
  // configured key, because it is what carries that key. It is taken here,
  // before the generic authentication below would drop it for having a MAC we
  // cannot verify yet.
  if (cmd == iohome::CMD_SEND_1W_KEY) {
    this->handle_1w_key_transfer_(data, frame_len, src_addr);
    return;
  }

  // Authenticate before anything is allowed to act on this frame.
  //
  // `position_feedback` used to apply whatever position a frame claimed, with
  // no check at all: anyone within radio range could park a cover's reported
  // state wherever they liked, and a recording of a genuine "closed" report
  // replayed forever. The MAC and the rolling code are what stop that, and the
  // protocol layer already knows how to check both.
  iohome::frame::IoFrame parsed;
  const bool parsed_ok = iohome::frame::parse_frame(data, frame_len, &parsed);

  // 0x3C / 0x3D drive the 2W session and are not covers' business. They are
  // taken before the generic check below because they verify against a nonce
  // that generic code cannot know: a 0x3C carries the challenger's own.
  if (this->two_way_ && (cmd == iohome::CMD_CHALLENGE_REQUEST ||
                         cmd == iohome::CMD_CHALLENGE_RESPONSE)) {
    // Only what is addressed to us. A house can hold several io-homecontrol
    // systems, and answering a challenge meant for a neighbouring hub tells
    // that hub nothing while announcing this one to anyone listening.
    if (dest_addr != this->source_address_) {
      ESP_LOGV(TAG, "2W frame 0x%02X for 0x%06X, not us", cmd,
               static_cast<unsigned int>(dest_addr));
      return;
    }
    if (parsed_ok) {
      this->handle_challenge_frame_(&parsed);
    }
    return;
  }

  bool authenticated = false;
  if (parsed_ok && parsed.authenticated) {
    // A 2W MAC is bound to the nonce of the session it belongs to, so the
    // check needs that nonce - without it validate_frame() cannot verify a 2W
    // frame at all and returns false. Passing nothing meant that with
    // `two_way: true` no incoming 2W frame could ever authenticate: every one
    // of them was counted as a MAC error and dropped, so position feedback
    // never worked in 2W mode.
    const uint8_t *challenge = nullptr;
    if (!parsed.is_1w_mode) {
      if (!this->auth_.has_active_challenge(millis())) {
        ESP_LOGD(TAG, "2W frame from 0x%06X with no session to check it against",
                 static_cast<unsigned int>(src_addr));
        // Fall through: validate_frame() will refuse, which is the right
        // answer - an unbound 2W MAC proves nothing.
      } else {
        challenge = this->auth_.get_current_challenge();
      }
    }

    if (!this->system_key_set_) {
      ESP_LOGD(TAG, "Frame carries a MAC but no system_key is configured");
    } else if (!iohome::frame::validate_frame(&parsed, this->system_key_, challenge)) {
      ESP_LOGW(TAG, "MAC verification failed for frame from 0x%06X",
               static_cast<unsigned int>(src_addr));
      this->mac_errors_++;
    } else if (!this->frame_is_fresh_(parsed)) {
      ESP_LOGW(TAG, "Replayed frame from 0x%06X", static_cast<unsigned int>(src_addr));
      this->replay_errors_++;
    } else {
      authenticated = true;
    }
  }

  ReceivedFrame frame{};
  frame.authenticated = authenticated;
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

void IOWNHomeControlComponent::handle_1w_key_transfer_(const uint8_t *data, size_t frame_len,
                                                       uint32_t src_addr) {
  if (!this->key_capture_) {
    ESP_LOGI(TAG, "1W key transfer seen from 0x%06X - set key_capture: true to read it",
             static_cast<unsigned int>(src_addr));
    return;
  }

  // The real layout, confirmed against captured 0x30 frames once they are
  // de-framed (docs/devices/velux/velux-frame-analysis.md, docs/commands.md):
  //
  //   ctrl0 ctrl1 | dest(3) | src(3) | cmd(1) | key(16) | mfr(1) | ?(1) | seq(2) | crc(2)
  //
  // Total 31 bytes. There is **no MAC**. Earlier code here expected one -
  // key(16) | seq(2) | MAC(6) | CRC(2), needing 35 bytes - and would have
  // rejected every real transfer as too short. It also claimed to "verify the
  // recovered key against its own MAC", which cannot be done: the frame carries
  // no MAC to check against. Only the CRC (already validated) protects it.
  constexpr size_t KEY_OFFSET = 9;
  constexpr size_t EXPECTED_LEN = KEY_OFFSET + iohome::AES_KEY_SIZE + 1 /*mfr*/ + 1 /*?*/ +
                                  iohome::ROLLING_CODE_SIZE + iohome::CRC_SIZE;  // 31
  if (frame_len < EXPECTED_LEN) {
    ESP_LOGW(TAG, "1W key transfer too short: %u bytes, expected %u",
             static_cast<unsigned>(frame_len), static_cast<unsigned>(EXPECTED_LEN));
    return;
  }

  const uint8_t *ciphertext = &data[KEY_OFFSET];
  const uint8_t manufacturer = data[KEY_OFFSET + iohome::AES_KEY_SIZE];
  const uint8_t node[iohome::NODE_ID_SIZE] = {data[5], data[6], data[7]};

  // The mask is AES(TRANSFER_KEY, IV) with the IV built from the node address
  // the key is addressed to. This reproduces the reference vector in
  // docs/linklayer.md, but that vector is synthetic: it has not been confirmed
  // that a real Velux 0x30 masks the same way. So the result below is a
  // *candidate*, printed for offline checking - not a key to trust blindly.
  uint8_t recovered[iohome::AES_KEY_SIZE];
  if (!iohome::crypto::decrypt_1w_key(ciphertext, node, recovered)) {
    ESP_LOGW(TAG, "1W key transfer: unmasking failed");
    return;
  }

  const auto to_hex = [](const uint8_t *b, size_t n) {
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
      char buf[3];
      snprintf(buf, sizeof(buf), "%02x", b[i]);
      s += buf;
    }
    return s;
  };

  // SECURITY: the bytes below are (a candidate for) the installation's 128-bit
  // system key - the secret that authenticates every command to every actuator.
  // key_capture: true opts into surfacing it, but ESP_LOGx is forwarded over
  // ESPHome's network API/logger and anything recording that stream, not just
  // the serial console the operator is watching. Lead with a loud warning so the
  // value is never pasted into a GitHub issue, forum post, or shared log.
  ESP_LOGW(TAG, "SECURITY: a system-key candidate follows. It IS the installation secret;");
  ESP_LOGW(TAG, "  anyone who reads it can command every actuator. These log lines travel over");
  ESP_LOGW(TAG, "  the network API too - do NOT share them. Move the node to serial-only logging");
  ESP_LOGW(TAG, "  while capturing, and clear the value from any dashboard/log afterwards.");
  ESP_LOGI(TAG, "1W key transfer from 0x%06X (manufacturer 0x%02X)",
           static_cast<unsigned int>(src_addr), manufacturer);
  ESP_LOGI(TAG, "  encrypted key on air: %s", to_hex(ciphertext, iohome::AES_KEY_SIZE).c_str());
  ESP_LOGI(TAG, "  de-masked candidate:  %s", to_hex(recovered, iohome::AES_KEY_SIZE).c_str());
  ESP_LOGI(TAG, "  UNVERIFIED: the frame carries no MAC to confirm this, and the masking may");
  ESP_LOGI(TAG, "  differ for this manufacturer. Confirm by checking that a later authenticated");
  ESP_LOGI(TAG, "  command from a known device validates under it before putting it in YAML.");

  iohome::crypto::secure_zero(recovered, sizeof(recovered));
}

void IOWNHomeControlComponent::dispatch_to_covers_(const ReceivedFrame &frame) {
  if (!this->position_feedback_) {
    return;
  }

  // An unauthenticated frame may be logged; it may not move a cover's state.
  // Without this, anyone in radio range could park a cover wherever they liked
  // - and a recording of a genuine report would keep working forever, because
  // nothing checked the rolling code either.
  if (!frame.authenticated) {
    ESP_LOGD(TAG, "Ignoring unauthenticated position report from 0x%06X",
             static_cast<unsigned int>(frame.src_address));
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

  // Wrap the frame in the on-air UART framing the receiver expects: each byte
  // becomes a start bit, its eight data bits least-significant first, and a
  // stop bit. A device that does not de-frame - as every real io-homecontrol
  // node does - would read our un-framed bytes as a garbage-length frame with
  // no valid CRC and drop it. See iohome_phy_framing.h.
  uint8_t wire[iohome::phy::uart_wire_size(IOHC_MAX_FRAME_SIZE)];
  const size_t wire_len = iohome::phy::uart_encode(data, len, wire, sizeof wire);
  if (wire_len == 0) {
    ESP_LOGE(TAG, "Could not frame %u bytes for transmit", static_cast<unsigned>(len));
    return false;
  }

  // Stop the interrupt from firing while we own the radio, and drop any packet
  // that arrived just before: its data is gone once we transmit.
  this->phy_->clearPacketReceivedAction();
  clear_packet_flag_();

  // In fixed-length mode the radio sends exactly the programmed number of
  // bytes, so narrow it to this frame's wire length and widen it again for
  // reception.
  int16_t state = this->set_packet_length_(static_cast<uint8_t>(wire_len));
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "Could not set packet length: %d", state);
  }

  state = this->phy_->transmit(wire, wire_len);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "Transmit failed: %d", state);
  }

  this->set_packet_length_(IOHC_RX_CAPTURE_SIZE);

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

  // 2W authenticates against a nonce the peer chose, so it needs a handshake
  // first and a completely different frame. The hand-rolled builder below is
  // the 1W path; the protocol layer builds the 2W one.
  if (this->two_way_) {
    if (command != IOHC_CMD_EXECUTE) {
      ESP_LOGW(TAG, "Only command 0x00 is implemented for 2W");
      return false;
    }
    return this->send_2w_command_(target_address, main_param, fp1, fp2);
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
