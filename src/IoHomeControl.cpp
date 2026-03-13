/**
 * @file IoHomeControl.cpp
 * @brief io-homecontrol Node Controller Implementation
 * @author iown-homecontrol project
 */

#include "IoHomeControl.h"
#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #define LOG_PRINT(x) if (verbose_) Serial.println(x)
  #define LOG_PRINTF(fmt, ...) if (verbose_) Serial.printf(fmt, ##__VA_ARGS__)
#else
  #include <stdio.h>
  #define LOG_PRINT(x) if (verbose_) printf("%s\n", x)
  #define LOG_PRINTF(fmt, ...) if (verbose_) printf(fmt, ##__VA_ARGS__)
#endif

namespace iohome {

IoHomeControl::IoHomeControl(PhysicalLayer* radio)
  : radio_(radio),
    rx_callback_(nullptr),
    rolling_code_(0),
    initialized_(false),
    receiving_(false),
    verbose_(false),
    channel_hopper_(nullptr),
    auth_manager_(nullptr),
    beacon_handler_(nullptr),
    discovery_manager_(nullptr)
{
  memset(own_node_id_, 0, NODE_ID_SIZE);
  memset(system_key_, 0, AES_KEY_SIZE);
  is_1w_mode_ = true;
}

bool IoHomeControl::begin(
  const uint8_t own_node_id[NODE_ID_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  bool is_1w
) {
  memcpy(own_node_id_, own_node_id, NODE_ID_SIZE);
  memcpy(system_key_, system_key, AES_KEY_SIZE);
  is_1w_mode_ = is_1w;

  LOG_PRINTF("IoHomeControl: Initializing (%s mode)\n", is_1w ? "1W" : "2W");
  LOG_PRINTF("  Node ID: %02X %02X %02X\n",
             own_node_id_[0], own_node_id_[1], own_node_id_[2]);

  // Initialize 2W components if in 2W mode
  if (!is_1w_mode_) {
    LOG_PRINT("Initializing 2W mode components...");

    channel_hopper_ = new mode2w::ChannelHopper();
    channel_hopper_->begin(CHANNEL_HOP_TIME_MS);

    auth_manager_ = new mode2w::AuthenticationManager();
    auth_manager_->begin(system_key_);

    beacon_handler_ = new mode2w::BeaconHandler();
    beacon_handler_->begin();

    discovery_manager_ = new mode2w::DiscoveryManager();
    discovery_manager_->begin(own_node_id_);

    LOG_PRINT("2W mode components initialized");
  }

  initialized_ = true;
  return true;
}

int16_t IoHomeControl::configure_radio(float frequency) {
  if (radio_ == nullptr) {
    LOG_PRINT("Error: Radio not initialized");
    return RADIOLIB_ERR_INVALID_RADIO;
  }

  LOG_PRINTF("Configuring radio on %.2f MHz\n", frequency);

  // Set frequency
  int16_t state = radio_->setFrequency(frequency);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setFrequency failed (%d)\n", state);
    return state;
  }

  // Set output power (start high and decrease until accepted)
  int8_t power = 20;
  do {
    state = radio_->setOutputPower(power--);
  } while (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER && power >= 0);

  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setOutputPower failed (%d)\n", state);
    return state;
  }

  // Set data rate (38.4 kbps, 19.2 kHz deviation)
  DataRate_t data_rate;
  data_rate.fsk.bitRate = BIT_RATE;  // 38.4 kbps
  data_rate.fsk.freqDev = FREQ_DEVIATION;  // 19.2 kHz

  state = radio_->setDataRate(data_rate);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setDataRate failed (%d)\n", state);
    return state;
  }

  // Set encoding and shaping
  state = radio_->setEncoding(RADIOLIB_ENCODING_NRZ);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setEncoding failed (%d)\n", state);
    return state;
  }

  state = radio_->setDataShaping(RADIOLIB_SHAPING_NONE);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setDataShaping failed (%d)\n", state);
    return state;
  }

  // Set sync word (0xFF33, 3 bytes)
  uint8_t sync_word[SYNC_WORD_LEN];
  sync_word[0] = (SYNC_WORD >> 16) & 0xFF;
  sync_word[1] = (SYNC_WORD >> 8) & 0xFF;
  sync_word[2] = SYNC_WORD & 0xFF;

  state = radio_->setSyncWord(sync_word, SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setSyncWord failed (%d)\n", state);
    return state;
  }

  // Set preamble length (512 bits = 64 bytes)
  state = radio_->setPreambleLength(PREAMBLE_LENGTH / 8);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setPreambleLength failed (%d)\n", state);
    return state;
  }

  LOG_PRINT("Radio configured successfully");
  return RADIOLIB_ERR_NONE;
}

int16_t IoHomeControl::start_receive(FrameReceivedCallback callback) {
  if (!initialized_) {
    LOG_PRINT("Error: Not initialized");
    return RADIOLIB_ERR_CHIP_NOT_FOUND;
  }

  rx_callback_ = callback;

  int16_t state = radio_->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: startReceive failed (%d)\n", state);
    return state;
  }

  receiving_ = true;
  LOG_PRINT("Receiving started");
  return RADIOLIB_ERR_NONE;
}

void IoHomeControl::stop_receive() {
  if (receiving_) {
    radio_->standby();
    receiving_ = false;
    LOG_PRINT("Receiving stopped");
  }
}

bool IoHomeControl::check_received(frame::IoFrame* frame, int16_t* rssi, float* snr) {
  if (!receiving_) {
    return false;
  }

  // Check if packet is available
  int16_t state = radio_->scanChannel();
  if (state != RADIOLIB_PREAMBLE_DETECTED) {
    return false;
  }

  // Read packet
  uint8_t buffer[FRAME_MAX_SIZE];
  int16_t len = radio_->readData(buffer, sizeof(buffer));

  if (len < 0) {
    LOG_PRINTF("Error: readData failed (%d)\n", len);
    return false;
  }

  // Parse frame
  if (!frame::parse_frame(buffer, len, frame)) {
    LOG_PRINT("Error: Frame parsing failed");
    return false;
  }

  // Validate frame (CRC and optionally HMAC)
  if (!frame::validate_frame(frame, system_key_)) {
    LOG_PRINT("Error: Frame validation failed");
    return false;
  }

  // Get RSSI and SNR
  int16_t rssi_val = radio_->getRSSI();
  float snr_val = radio_->getSNR();

  if (rssi != nullptr) {
    *rssi = rssi_val;
  }
  if (snr != nullptr) {
    *snr = snr_val;
  }

  LOG_PRINT("Frame received successfully");

  // Process frame internally (beacons, discovery, auth)
  process_received_frame(frame, rssi_val, snr_val);

  // Call callback if set
  if (rx_callback_ != nullptr) {
    rx_callback_(frame, rssi_val, snr_val);
  }

  return true;
}

bool IoHomeControl::send_command(
  const uint8_t dest_node[NODE_ID_SIZE],
  uint8_t cmd_id,
  const uint8_t* params,
  size_t params_len
) {
  if (!initialized_) {
    LOG_PRINT("Error: Not initialized");
    return false;
  }

  // Create frame
  frame::IoFrame tx_frame;
  frame::init_frame(&tx_frame, is_1w_mode_);
  frame::set_destination(&tx_frame, dest_node);
  frame::set_source(&tx_frame, own_node_id_);

  if (!frame::set_command(&tx_frame, cmd_id, params, params_len)) {
    LOG_PRINT("Error: set_command failed");
    return false;
  }

  // Set rolling code (1W mode only)
  if (is_1w_mode_) {
    frame::set_rolling_code(&tx_frame, rolling_code_);
    rolling_code_++; // Increment for next transmission
  }

  // Finalize frame (calculate HMAC and CRC)
  if (!frame::finalize_frame(&tx_frame, system_key_)) {
    LOG_PRINT("Error: finalize_frame failed");
    return false;
  }

  // Transmit frame
  return transmit_frame(&tx_frame);
}

bool IoHomeControl::set_position(const uint8_t dest_node[NODE_ID_SIZE], uint8_t position) {
  LOG_PRINTF("Setting position to %d%%\n", position);

  uint8_t params[2] = {position, 0x00};
  return send_command(dest_node, CMD_SET_POSITION, params, 2);
}

bool IoHomeControl::open(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_PRINT("Opening actuator");
  return set_position(dest_node, 100);
}

bool IoHomeControl::close(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_PRINT("Closing actuator");
  return set_position(dest_node, 0);
}

bool IoHomeControl::stop(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_PRINT("Stopping actuator");

  uint8_t params[1] = {0x00};
  return send_command(dest_node, CMD_STOP, params, 1);
}

int16_t IoHomeControl::get_rssi() {
  return radio_->getRSSI();
}

float IoHomeControl::get_snr() {
  return radio_->getSNR();
}

bool IoHomeControl::transmit_frame(const frame::IoFrame* frame) {
  // Serialize frame
  uint8_t buffer[FRAME_MAX_SIZE];
  size_t len = frame::serialize_frame(frame, buffer, sizeof(buffer));

  if (len == 0) {
    LOG_PRINT("Error: serialize_frame failed");
    return false;
  }

  if (verbose_) {
    LOG_PRINTF("Transmitting %d bytes:\n", len);
    for (size_t i = 0; i < len; i++) {
      LOG_PRINTF("%02X ", buffer[i]);
      if ((i + 1) % 16 == 0) LOG_PRINT("");
    }
    LOG_PRINT("");
  }

  // Stop receiving if active
  bool was_receiving = receiving_;
  if (was_receiving) {
    stop_receive();
  }

  // Transmit
  int16_t state = radio_->transmit(buffer, len);

  // Resume receiving if it was active
  if (was_receiving) {
    start_receive(rx_callback_);
  }

  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: transmit failed (%d)\n", state);
    return false;
  }

  LOG_PRINT("Frame transmitted successfully");
  return true;
}

void IoHomeControl::log(const char* message) {
  if (verbose_) {
    LOG_PRINT(message);
  }
}

void IoHomeControl::process_received_frame(const frame::IoFrame* frame, int16_t rssi, float snr) {
  // Process beacons if in 2W mode
  if (!is_1w_mode_ && beacon_handler_ != nullptr) {
    if (beacon_handler_->process_beacon(frame, rssi, snr)) {
      LOG_PRINT("Beacon received");
    }
  }

  // Process discovery responses
  if (discovery_manager_ != nullptr) {
    discovery_manager_->process_discovery_response(frame, rssi);
  }

  // Process challenge responses if waiting for one
  if (!is_1w_mode_ && auth_manager_ != nullptr) {
    if (frame->command_id == CMD_CHALLENGE_RESPONSE) {
      if (auth_manager_->verify_challenge_response(frame)) {
        LOG_PRINT("Authentication successful");
      } else {
        LOG_PRINT("Authentication failed");
      }
    }
  }
}

// ============================================================================
// 2W Mode Features Implementation
// ============================================================================

bool IoHomeControl::enable_frequency_hopping(bool enable) {
  if (is_1w_mode_) {
    LOG_PRINT("Warning: Frequency hopping only available in 2W mode");
    return false;
  }

  if (channel_hopper_ == nullptr) {
    LOG_PRINT("Error: Channel hopper not initialized");
    return false;
  }

  channel_hopper_->set_enabled(enable);
  LOG_PRINTF("Frequency hopping %s\n", enable ? "enabled" : "disabled");
  return true;
}

bool IoHomeControl::update_frequency_hopping() {
  if (is_1w_mode_ || channel_hopper_ == nullptr || !channel_hopper_->is_enabled()) {
    return false;
  }

#ifdef ARDUINO
  unsigned long current_time = millis();
#else
  unsigned long current_time = GET_TIME_MS();
#endif

  if (channel_hopper_->update(current_time)) {
    // Channel switched, reconfigure radio
    float new_freq = channel_hopper_->get_current_frequency();
    int16_t state = radio_->setFrequency(new_freq);

    if (state == RADIOLIB_ERR_NONE) {
      LOG_PRINTF("Switched to channel: %.2f MHz\n", new_freq);
      return true;
    } else {
      LOG_PRINTF("Error: Failed to switch channel (%d)\n", state);
      return false;
    }
  }

  return false;
}

mode2w::ChannelState IoHomeControl::get_current_channel() const {
  if (channel_hopper_ == nullptr) {
    return mode2w::ChannelState::CHANNEL_2;
  }
  return channel_hopper_->get_current_channel();
}

bool IoHomeControl::send_challenge_request(const uint8_t dest_node[NODE_ID_SIZE]) {
  if (is_1w_mode_) {
    LOG_PRINT("Error: Challenge-response only available in 2W mode");
    return false;
  }

  if (auth_manager_ == nullptr) {
    LOG_PRINT("Error: Authentication manager not initialized");
    return false;
  }

  frame::IoFrame frame;
  if (!auth_manager_->create_challenge_request(&frame, dest_node, own_node_id_)) {
    LOG_PRINT("Error: Failed to create challenge request");
    return false;
  }

  LOG_PRINT("Sending challenge request");
  return transmit_frame(&frame);
}

bool IoHomeControl::send_challenge_response(const uint8_t dest_node[NODE_ID_SIZE], const uint8_t challenge[HMAC_SIZE]) {
  if (is_1w_mode_) {
    LOG_PRINT("Error: Challenge-response only available in 2W mode");
    return false;
  }

  if (auth_manager_ == nullptr) {
    LOG_PRINT("Error: Authentication manager not initialized");
    return false;
  }

  frame::IoFrame frame;
  if (!auth_manager_->create_challenge_response(&frame, dest_node, own_node_id_, challenge)) {
    LOG_PRINT("Error: Failed to create challenge response");
    return false;
  }

  LOG_PRINT("Sending challenge response");
  return transmit_frame(&frame);
}

mode2w::ChallengeState IoHomeControl::get_auth_state() const {
  if (auth_manager_ == nullptr) {
    return mode2w::ChallengeState::IDLE;
  }
  return auth_manager_->get_state();
}

void IoHomeControl::start_discovery(uint8_t device_type, unsigned long timeout_ms) {
  if (discovery_manager_ == nullptr) {
    discovery_manager_ = new mode2w::DiscoveryManager();
    discovery_manager_->begin(own_node_id_);
  }

  LOG_PRINTF("Starting discovery (device type: 0x%02X, timeout: %lu ms)\n", device_type, timeout_ms);
  discovery_manager_->start_discovery(device_type, timeout_ms);

  // Send discovery request
  frame::IoFrame frame;
  if (discovery_manager_->create_discovery_request(&frame, device_type)) {
    // Note: Discovery doesn't use encryption
    transmit_frame(&frame);
  }
}

void IoHomeControl::stop_discovery() {
  if (discovery_manager_ != nullptr) {
    discovery_manager_->stop_discovery();
    LOG_PRINT("Discovery stopped");
  }
}

size_t IoHomeControl::get_discovered_count() const {
  if (discovery_manager_ == nullptr) {
    return 0;
  }
  return discovery_manager_->get_discovered_count();
}

bool IoHomeControl::get_discovered_device(size_t index, mode2w::DiscoveredDevice* device) {
  if (discovery_manager_ == nullptr) {
    return false;
  }
  return discovery_manager_->get_discovered_device(index, device);
}

bool IoHomeControl::pair_device_1w(const uint8_t dest_node[NODE_ID_SIZE], const uint8_t new_system_key[AES_KEY_SIZE]) {
  if (discovery_manager_ == nullptr) {
    discovery_manager_ = new mode2w::DiscoveryManager();
    discovery_manager_->begin(own_node_id_);
  }

  LOG_PRINT("Pairing device (1W mode)");

  frame::IoFrame frame;
  if (!discovery_manager_->create_key_transfer_1w(&frame, dest_node, own_node_id_, new_system_key)) {
    LOG_PRINT("Error: Failed to create key transfer frame");
    return false;
  }

  // Finalize frame without existing key (using transfer key)
  if (!frame::finalize_frame(&frame, TRANSFER_KEY)) {
    LOG_PRINT("Error: Failed to finalize key transfer frame");
    return false;
  }

  return transmit_frame(&frame);
}

bool IoHomeControl::pair_device_2w(const uint8_t dest_node[NODE_ID_SIZE], const uint8_t new_system_key[AES_KEY_SIZE]) {
  if (!auth_manager_ || discovery_manager_ == nullptr) {
    LOG_PRINT("Error: 2W components not initialized");
    return false;
  }

  LOG_PRINT("Pairing device (2W mode)");

  // Generate challenge
  uint8_t challenge[HMAC_SIZE];
  auth_manager_->generate_challenge(challenge);

  frame::IoFrame frame;
  if (!discovery_manager_->create_key_transfer_2w(&frame, dest_node, own_node_id_, new_system_key, challenge)) {
    LOG_PRINT("Error: Failed to create key transfer frame");
    return false;
  }

  // Finalize frame with challenge
  if (!frame::finalize_frame(&frame, TRANSFER_KEY, challenge)) {
    LOG_PRINT("Error: Failed to finalize key transfer frame");
    return false;
  }

  return transmit_frame(&frame);
}

bool IoHomeControl::has_recent_beacon(unsigned long timeout_ms) {
  if (beacon_handler_ == nullptr) {
    return false;
  }
  return beacon_handler_->has_recent_beacon(timeout_ms);
}

bool IoHomeControl::get_last_beacon(mode2w::BeaconInfo* info) {
  if (beacon_handler_ == nullptr) {
    return false;
  }
  return beacon_handler_->get_last_beacon(info);
}

} // namespace iohome
