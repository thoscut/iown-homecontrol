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

  // Position reports are only ever taken from the configured covers, so tell
  // the replay guard exactly those and nobody else. Without it, a recorded
  // report re-addressed to a made-up source (the MAC does not cover addresses;
  // the CRC is trivial to recompute) was a fresh "first frame" every time.
  // The covers themselves are registered as known peers by register_cover():
  // they set up after this component, so covers_ is still empty here.
  if (this->position_feedback_) {
    this->replay_guard_.set_strict(true);
  }

  // Set up interrupt-driven receive
  this->phy_->setPacketReceivedAction(packet_isr_);
  clear_packet_flag_();

  state = this->phy_->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "Failed to start receive mode: %d", state);
  }

  ESP_LOGI(TAG, "io-homecontrol initialized on %.2f MHz", this->frequency_);
}

void IOWNHomeControlComponent::register_cover(IOWNCover *cover) {
  if (cover == nullptr) {
    return;
  }
  this->covers_.push_back(cover);

  if (!this->position_feedback_) {
    return;
  }
  uint8_t node[iohome::NODE_ID_SIZE];
  address_to_node(cover->get_target_address(), node);
  if (!iohome::frame::is_broadcast(node) && !this->replay_guard_.add_known_peer(node)) {
    ESP_LOGW(TAG, "Replay guard full: position reports from 0x%06X will be rejected",
             static_cast<unsigned int>(cover->get_target_address()));
  }
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

  // Set Tx output power (start here, decrease until supported). Kept modest: the
  // GC1109 front-end adds gain on top, and driving the SX126x at 20 dBm into it
  // browns out a USB-powered board mid-transmit (the serial link drops and the
  // ESP resets). 10 dBm + the PA still reaches across a house with margin.
  int8_t pwr = 10;
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
  return iohome::mp_from_percent_closed(percent_closed);
}

uint8_t IOWNHomeControlComponent::percent_closed_from_main_param(uint16_t main_param) {
  // 0xFF for a function code such as 0xD200 (stop), which is not a percentage.
  return iohome::percent_closed_from_mp(main_param);
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

  // Seed the counter when impersonating (or re-adopting) an existing controller:
  // the actuator remembers that node's last sequence, so we must start above it
  // or every command reads as a replay. Only ever raises, never lowers.
  if (stored < this->initial_rolling_code_) {
    ESP_LOGI(TAG, "Seeding rolling code to configured initial %u",
             static_cast<unsigned>(this->initial_rolling_code_));
    stored = this->initial_rolling_code_;
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
    const bool saved = this->rolling_code_pref_.save(&this->rolling_code_reserved_until_);
    // save() only queues the write; ESPHome commits the queue on its
    // flash_write_interval timer (60 s by default) or on a clean shutdown. A
    // brownout or crash before that would hand out the same codes again after
    // the reboot, and the actuator rejects them as replays. Commit now - this
    // runs once per reserved block, so the flash wear stays negligible.
    if (!saved || !global_preferences->sync()) {
      // Nothing better to do than shout: refusing to transmit would leave the
      // cover dead. After a reboot the codes since the last good write repeat
      // and the actuator ignores them until the counter catches up.
      ESP_LOGE(TAG, "Could not persist the rolling-code reservation (0x%04X)",
               this->rolling_code_reserved_until_);
      // Do not trust a reservation that is not on flash: leaving it in place
      // skipped the write for the rest of the block, so a transient failure
      // kept up to a block of codes unpersisted. Make the next call retry
      // (remaining wraps past the block), as src/IoHomeControl.cpp does.
      this->rolling_code_reserved_until_ = this->rolling_code_;
    }
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
// docs/linklayer.md, "2W Authentication": 2W frames carry no MAC. A command
// goes out plain; the actuator answers it with a plain 0x3C carrying a fresh
// 6-byte challenge; we answer with a plain 0x3D whose payload is the MAC over
// the command we sent and that challenge; then the actuator executes. The
// reverse direction is the same with the roles swapped.
//
// This used to be modelled as a session we opened with our own 0x3C, MAC'd
// trailers on every 2W frame, and a 0x3D echoing the challenge - none of which
// a real actuator sends or accepts.
// ---------------------------------------------------------------------------

bool IOWNHomeControlComponent::frame_is_fresh_(const iohome::frame::IoFrame &frame) {
  if (frame.is_1w_mode) {
    // The MAC goes in with the sequence: it is not bound to the addresses, so
    // the guard's global MAC ring is what catches a re-addressed replay.
    return this->replay_guard_.accept(frame.src_node, iohome::frame::get_rolling_code(&frame),
                                      frame.hmac);
  }

  // 2W frames carry no MAC trailer, so none reaches here as "authenticated";
  // their freshness is the nonce in the 0x3C/0x3D exchange.
  return false;
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

bool IOWNHomeControlComponent::send_2w_command_(uint32_t target_address, uint16_t main_param,
                                                uint8_t fp1, uint8_t fp2) {
  if (!this->system_key_set_) {
    // Without the key the actuator's 0x3C cannot be answered, so the command
    // would go out and never be executed.
    ESP_LOGE(TAG, "2W needs system_key to answer the actuator's challenge");
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
          static_cast<uint8_t>(this->acei_ | IOHC_ACEI_VALID_MASK), fp1, fp2) ||
      !iohome::frame::finalize_frame_plain(&frame)) {
    ESP_LOGE(TAG, "Rejected execute payload (ACEI 0x%02X?)", this->acei_);
    return false;
  }

  // Remember it: the actuator's 0x3C for this command is answered with a MAC
  // over exactly these bytes, and only within a short window.
  this->auth_.note_request_sent(&frame, millis());

  ESP_LOGD(TAG, "Sending 2W command: target=0x%06X main=0x%04X",
           static_cast<unsigned int>(target_address), main_param);
  if (!this->send_protocol_frame_(&frame)) {
    this->auth_.clear_pending_request();
    return false;
  }
  return true;
}

void IOWNHomeControlComponent::handle_challenge_frame_(const iohome::frame::IoFrame *frame,
                                                       int16_t rssi) {
  const uint32_t now = millis();

  if (frame->command_id == iohome::CMD_CHALLENGE_REQUEST) {
    // The actuator challenges a command. Answer only if it is OUR pending
    // command to that actuator, sent moments ago: signing a nonce for anyone
    // who asks would hand out MACs over attacker-chosen input.
    iohome::frame::IoFrame response;
    if (!this->auth_.answer_challenge(frame, &response, now)) {
      ESP_LOGD(TAG, "Challenge from 0x%02X%02X%02X is not for a pending command; not answering",
               frame->src_node[0], frame->src_node[1], frame->src_node[2]);
      return;
    }
    ESP_LOGD(TAG, "Answering 2W challenge");
    this->send_protocol_frame_(&response);
    return;
  }

  if (frame->command_id == iohome::CMD_CHALLENGE_RESPONSE) {
    // The answer to a challenge we sent for a command a peer sent us.
    iohome::frame::IoFrame verified;
    if (!this->auth_.verify_challenge_response(frame, now) ||
        !this->auth_.take_verified_request(&verified)) {
      ESP_LOGW(TAG, "2W challenge answer rejected");
      this->mac_errors_++;
      return;
    }
    ESP_LOGI(TAG, "2W command from 0x%02X%02X%02X authenticated", verified.src_node[0],
             verified.src_node[1], verified.src_node[2]);

    ReceivedFrame rf{};
    rf.authenticated = true;
    rf.ctrl0 = verified.ctrl_byte_0;
    rf.ctrl1 = verified.ctrl_byte_1;
    rf.dest_address = node_to_address(verified.dest_node);
    rf.src_address = node_to_address(verified.src_node);
    rf.command = verified.command_id;
    rf.payload = verified.data;
    rf.payload_len = verified.data_len;
    rf.is_1w = false;
    rf.rssi = rssi;
    this->handle_execute_(rf);
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

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  // The raw on-air bytes, framing and all, before de-framing. This is what a
  // capture session records; it is logged at VERBOSE so it does not drown the
  // ordinary DEBUG frame log. It is logged after de-framing so a key transfer
  // can be recognised and kept out of the log.
  if (this->may_log_frame_contents_(frame, frame_len)) {
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

  this->parse_frame_(frame, frame_len, rssi);
}

bool IOWNHomeControlComponent::may_log_frame_contents_(const uint8_t *data, size_t len) const {
  // A 0x30/0x32 key transfer is masked only with the public transfer key and the
  // sender's address, so a hex dump of it is the system key in all but name.
  // The DEBUG examples stream logs to Home Assistant, which made pairing a remote
  // near the hub leak the key even with key_capture off.
  if (this->key_capture_ || len <= 8) {
    return true;
  }
  return !iohome::frame::carries_key_material(data[8]);
}

void IOWNHomeControlComponent::parse_frame_(const uint8_t *data, size_t len, int16_t rssi) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  // The de-framed frame, as the protocol layer sees it. The raw on-air bytes
  // are logged separately at VERBOSE in receive_frame_().
  if (this->may_log_frame_contents_(data, len)) {
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
  const uint32_t dest_addr = node_to_address(&data[2]);
  const uint32_t src_addr = node_to_address(&data[5]);
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

  // The frame's bytes were already dumped at the top of this function
  // ("RX frame ..."): receive_frame_() hands over exactly the declared length.

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

  // 0x3C / 0x3D are the 2W authentication exchange and not covers' business.
  // Both are plain frames; what they prove is checked by the auth manager.
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
      this->handle_challenge_frame_(&parsed, rssi);
    }
    return;
  }

  // A plain 2W command addressed to us is authenticated the 2W way: hold it,
  // challenge the sender, act on it only once its 0x3D verifies (handled
  // above). Only worth doing when something would act on it.
  if (this->two_way_ && parsed_ok && !parsed.is_1w_mode && !parsed.authenticated &&
      dest_addr == this->source_address_ && this->position_feedback_ &&
      this->system_key_set_ && !iohome::frame::is_unauthenticated_command(cmd)) {
    iohome::frame::IoFrame chal;
    uint8_t own[iohome::NODE_ID_SIZE];
    address_to_node(this->source_address_, own);
    if (this->auth_.create_challenge_for(&chal, &parsed, own, millis())) {
      ESP_LOGD(TAG, "2W command from 0x%06X held; challenging it",
               static_cast<unsigned int>(src_addr));
      this->send_protocol_frame_(&chal);
    } else {
      ESP_LOGD(TAG, "2W command from 0x%06X dropped: a challenge is already pending",
               static_cast<unsigned int>(src_addr));
    }
    return;
  }

  bool authenticated = false;
  if (parsed_ok && parsed.authenticated) {
    // Only 1W frames carry a MAC trailer; the parser reads 2W frames as plain.
    if (!this->system_key_set_) {
      ESP_LOGD(TAG, "Frame carries a MAC but no system_key is configured");
    } else if (!iohome::frame::validate_frame(&parsed, this->system_key_)) {
      ESP_LOGW(TAG, "MAC verification failed for frame from 0x%06X",
               static_cast<unsigned int>(src_addr));
      this->mac_errors_++;
    } else if (!this->frame_is_fresh_(parsed)) {
      ESP_LOGW(TAG, "Replayed or unknown-source frame from 0x%06X",
               static_cast<unsigned int>(src_addr));
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

  this->handle_execute_(frame);
}

void IOWNHomeControlComponent::handle_execute_(const ReceivedFrame &frame) {
  if (frame.command != IOHC_CMD_EXECUTE || frame.payload == nullptr ||
      frame.payload_len < IOHC_EXEC_PAYLOAD_SIZE) {
    return;
  }

  // Payload layout, counted from the byte after the command:
  //   originator | ACEI | main parameter (2, MSB first) | FP1 | FP2
  // The IOHC_EXEC_OFFSET_* values count from the command byte itself.
  const uint8_t *p = frame.payload;
  const uint8_t originator = p[IOHC_EXEC_OFFSET_ORIGINATOR - 1];
  const uint8_t acei = p[IOHC_EXEC_OFFSET_ACEI - 1];
  const uint16_t main_param = static_cast<uint16_t>(
      (static_cast<uint16_t>(p[IOHC_EXEC_OFFSET_MAIN_PARAM - 1]) << 8) |
      p[IOHC_EXEC_OFFSET_MAIN_PARAM]);

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

  // 2W commands go out plain and are authenticated afterwards, by answering
  // the actuator's 0x3C. The hand-rolled builder below is the 1W path; the
  // protocol layer builds the 2W one.
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
   * Byte 15-16: Sequence number (2 bytes, high byte first)
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

  // Real 1W command frames carry order bits 11 (0xC0, "group end") in ctrl0; an
  // actuator ignores an otherwise-valid command that arrives as order 00. This
  // was verified byte-for-byte against a real remote's CLOSE: with 0xC0 our
  // frame is identical to the wire, without it only ctrl0 (and hence the CRC)
  // differed. send_1w_pairing()/send_1w_remove() already set these bits; the
  // command builder must too. The 2W-plain path keeps order 00.
  const uint8_t order_bits = authenticated ? 0xC0u : 0x00u;
  frame[pos++] = static_cast<uint8_t>(order_bits | (authenticated ? IOHC_MODE_1W : IOHC_MODE_2W) | size_field);
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
    // Observed Velux 1W frames carry the sequence high byte first: the low byte
    // is the one that increments per press. Match that so the actuator's
    // freshness check sees the same monotonic counter its own remotes send.
    // (The MAC is self-consistent either way, since sender and receiver both key
    // the IV off these two bytes in wire order - only freshness cares.)
    rc[0] = (sequence >> 8) & 0xFF;  // high byte first on the wire
    rc[1] = sequence & 0xFF;

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

bool IOWNHomeControlComponent::send_1w_pairing(uint32_t dest_address) {
  if (this->is_failed()) {
    return false;
  }
  if (!this->system_key_set_) {
    ESP_LOGE(TAG, "1W pairing needs a system_key - the ESP's own key to hand the actuator");
    return false;
  }

  // 0x30 "Send 1W Key" (31 bytes, no MAC - the receiver has no key yet):
  //   ctrl0 ctrl1 | dest(3) | src(3) | 0x30 | enc_key(16) | mfr | reserved | seq(2) | crc(2)
  // We transfer OUR OWN system_key, masked with OUR OWN source address (the
  // actuator de-masks by the sender's address). Sent to an actuator that has
  // been put in learn mode (the gear button on a KLI 31x switch), it stores us
  // as an additional 1W controller; our normal commands - signed with the same
  // key, under our own identity - are then accepted, without touching any real
  // remote's rolling code.
  uint8_t frame[IOHC_MAX_FRAME_SIZE];
  size_t pos = 0;

  constexpr size_t total_len = 9 + iohome::AES_KEY_SIZE + 1 + 1 + 2 + 2;  // 31
  const uint8_t size_field =
      static_cast<uint8_t>((total_len - IOHC_SIZE_BIAS) & IOHC_CTRL0_SIZE_MASK);

  // Order bits 11 (group-end), matching the real 0x30 on the wire (ctrl0 0xFC).
  frame[pos++] = static_cast<uint8_t>(0xC0 | IOHC_MODE_1W | size_field);
  frame[pos++] = 0x00;

  frame[pos++] = (dest_address >> 16) & 0xFF;
  frame[pos++] = (dest_address >> 8) & 0xFF;
  frame[pos++] = dest_address & 0xFF;

  const uint8_t src[iohome::NODE_ID_SIZE] = {
      static_cast<uint8_t>((this->source_address_ >> 16) & 0xFF),
      static_cast<uint8_t>((this->source_address_ >> 8) & 0xFF),
      static_cast<uint8_t>(this->source_address_ & 0xFF)};
  frame[pos++] = src[0];
  frame[pos++] = src[1];
  frame[pos++] = src[2];

  frame[pos++] = iohome::CMD_SEND_1W_KEY;  // 0x30

  uint8_t enc_key[iohome::AES_KEY_SIZE];
  if (!iohome::crypto::encrypt_1w_key(this->system_key_, src, enc_key)) {
    ESP_LOGE(TAG, "1W pairing: key masking failed");
    return false;
  }
  memcpy(&frame[pos], enc_key, sizeof(enc_key));
  pos += sizeof(enc_key);
  iohome::crypto::secure_zero(enc_key, sizeof(enc_key));

  frame[pos++] = 0x01;  // manufacturer (Velux), as seen on the wire
  frame[pos++] = 0x01;  // reserved, as seen on the wire

  const uint16_t sequence = this->consume_rolling_code_();
  frame[pos++] = (sequence >> 8) & 0xFF;  // high byte first, like commands
  frame[pos++] = sequence & 0xFF;

  const uint16_t crc = compute_crc(frame, pos);
  frame[pos++] = crc & 0xFF;
  frame[pos++] = (crc >> 8) & 0xFF;

  if (pos != total_len) {
    ESP_LOGE(TAG, "Internal error: pairing frame %u vs %u", static_cast<unsigned>(pos),
             static_cast<unsigned>(total_len));
    return false;
  }

  ESP_LOGI(TAG, "Sending 1W pairing (0x30) as 0x%06X to 0x%06X (seq=%u) - actuator must be in learn mode",
           static_cast<unsigned int>(this->source_address_),
           static_cast<unsigned int>(dest_address), static_cast<unsigned>(sequence));
  return this->send_frame(frame, pos);
}

bool IOWNHomeControlComponent::send_1w_remove(uint32_t dest_address) {
  if (this->is_failed()) {
    return false;
  }
  if (!this->system_key_set_) {
    ESP_LOGE(TAG, "1W remove needs a system_key");
    return false;
  }

  // 0x39 "Remove 1W controller" (20 bytes), authenticated:
  //   ctrl0 ctrl1 | dest(3) | src(3) | 0x39 | 0x00 | seq(2) | mac(6) | crc(2)
  // The io 1W discovery handshake sends this (exclusion) just before the 0x30
  // key transfer; the actuator forgets any old controller at this address, then
  // learns our key from the following 0x30.
  uint8_t frame[IOHC_MAX_FRAME_SIZE];
  size_t pos = 0;

  constexpr size_t total_len = 9 + 1 + 2 + 6 + 2;  // 20
  const uint8_t size_field =
      static_cast<uint8_t>((total_len - IOHC_SIZE_BIAS) & IOHC_CTRL0_SIZE_MASK);

  frame[pos++] = static_cast<uint8_t>(0xC0 | IOHC_MODE_1W | size_field);  // order 11, 1W -> 0xF1
  frame[pos++] = 0x00;

  frame[pos++] = (dest_address >> 16) & 0xFF;
  frame[pos++] = (dest_address >> 8) & 0xFF;
  frame[pos++] = dest_address & 0xFF;

  frame[pos++] = (this->source_address_ >> 16) & 0xFF;
  frame[pos++] = (this->source_address_ >> 8) & 0xFF;
  frame[pos++] = this->source_address_ & 0xFF;

  frame[pos++] = iohome::CMD_REMOVE_1W_CONTROLLER;  // 0x39
  frame[pos++] = 0x00;                              // payload, as seen on the wire

  // MAC over command byte + payload (0x39 0x00), like every authenticated 1W frame.
  const uint8_t *hmac_data = &frame[8];
  const size_t hmac_data_len = 2;
  const uint16_t sequence = this->consume_rolling_code_();
  uint8_t rc[2];
  rc[0] = (sequence >> 8) & 0xFF;
  rc[1] = sequence & 0xFF;

  uint8_t hmac[6];
  if (!this->compute_hmac_(hmac_data, hmac_data_len, rc, hmac)) {
    ESP_LOGE(TAG, "1W remove: MAC computation failed");
    return false;
  }
  frame[pos++] = rc[0];
  frame[pos++] = rc[1];
  memcpy(&frame[pos], hmac, sizeof(hmac));
  pos += sizeof(hmac);

  const uint16_t crc = compute_crc(frame, pos);
  frame[pos++] = crc & 0xFF;
  frame[pos++] = (crc >> 8) & 0xFF;

  if (pos != total_len) {
    ESP_LOGE(TAG, "Internal error: remove frame %u vs %u", static_cast<unsigned>(pos),
             static_cast<unsigned>(total_len));
    return false;
  }

  ESP_LOGI(TAG, "Sending 1W remove (0x39) as 0x%06X to 0x%06X (seq=%u)",
           static_cast<unsigned int>(this->source_address_),
           static_cast<unsigned int>(dest_address), static_cast<unsigned>(sequence));
  return this->send_frame(frame, pos);
}

}  // namespace iown_homecontrol
}  // namespace esphome
