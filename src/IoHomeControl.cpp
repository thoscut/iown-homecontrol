/**
 * @file IoHomeControl.cpp
 * @brief io-homecontrol Node Controller Implementation
 * @author iown-homecontrol project
 */

#include "IoHomeControl.h"
#include <string.h>
#include <new>

#ifdef ARDUINO
  #include <Arduino.h>
  #define LOG_PRINT(x) do { if (verbose_) Serial.println(x); } while (0)
  #define LOG_PRINTF(fmt, ...) do { if (verbose_) Serial.printf(fmt, ##__VA_ARGS__); } while (0)
  #define NOW_MS() millis()
  #define NOW_US() micros()
#else
  #include <stdio.h>
  #include <time.h>
  #define LOG_PRINT(x) do { if (verbose_) printf("%s\n", x); } while (0)
  #define LOG_PRINTF(fmt, ...) do { if (verbose_) printf(fmt, ##__VA_ARGS__); } while (0)
  // clock() is signed; cast before widening so the division stays well defined.
  #define NOW_MS() \
    ((unsigned long)((unsigned long long)(clock()) * 1000ULL / (unsigned long long)(CLOCKS_PER_SEC)))
  #define NOW_US() \
    ((unsigned long)((unsigned long long)(clock()) * 1000000ULL / (unsigned long long)(CLOCKS_PER_SEC)))
#endif

#ifndef IRAM_ATTR
  #define IRAM_ATTR
#endif

namespace iohome {
namespace {

/// RadioLib's packet callback carries no user context, so the flag is static.
volatile bool g_packet_flag = false;

void IRAM_ATTR packet_isr() {
  g_packet_flag = true;
}

} // namespace

IoHomeControl::IoHomeControl(PhysicalLayer* radio)
  : radio_(radio),
    rx_callback_(nullptr),
    is_1w_mode_(true),
    rolling_code_(0),
    initialized_(false),
    receiving_(false),
    verbose_(false),
    accept_plain_frames_(false),
    originator_(Originator::USER),
    acei_(ACEI_DEFAULT),
    packet_length_callback_(nullptr),
    packet_length_context_(nullptr),
    rolling_code_store_(nullptr),
    rolling_code_reserved_until_(0),
    rolling_code_reserve_block_(64),
    last_reject_(RxReject::NONE),
    rx_stats_{},
    channel_hopper_(nullptr),
    auth_manager_(nullptr),
    beacon_handler_(nullptr),
    discovery_manager_(nullptr)
{
  memset(own_node_id_, 0, NODE_ID_SIZE);
  memset(system_key_, 0, AES_KEY_SIZE);
}

IoHomeControl::~IoHomeControl() {
  if (receiving_ && radio_ != nullptr) {
    radio_->clearPacketReceivedAction();
  }
  destroy_2w_components();
  crypto::secure_zero(system_key_, AES_KEY_SIZE);
}

void IoHomeControl::destroy_2w_components() {
  delete channel_hopper_;
  channel_hopper_ = nullptr;
  delete auth_manager_;
  auth_manager_ = nullptr;
  delete beacon_handler_;
  beacon_handler_ = nullptr;
  delete discovery_manager_;
  discovery_manager_ = nullptr;
}

bool IoHomeControl::begin(
  const uint8_t own_node_id[NODE_ID_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  bool is_1w
) {
  if (own_node_id == nullptr || system_key == nullptr) {
    LOG_PRINT("Error: Invalid parameters (nullptr)");
    return false;
  }

  if (radio_ == nullptr) {
    LOG_PRINT("Error: Radio not set");
    return false;
  }

  memcpy(own_node_id_, own_node_id, NODE_ID_SIZE);
  memcpy(system_key_, system_key, AES_KEY_SIZE);
  is_1w_mode_ = is_1w;

  LOG_PRINTF("IoHomeControl: Initializing (%s mode)\n", is_1w ? "1W" : "2W");
  LOG_PRINTF("  Node ID: %02X %02X %02X\n",
             own_node_id_[0], own_node_id_[1], own_node_id_[2]);

  // Clean up any existing 2W components before re-initialization.
  destroy_2w_components();

  // The discovery manager is useful in both modes: 1W pairing also relies on it.
  discovery_manager_ = new (std::nothrow) mode2w::DiscoveryManager();
  if (discovery_manager_ == nullptr) {
    LOG_PRINT("Error: Failed to allocate DiscoveryManager");
    return false;
  }
  discovery_manager_->begin(own_node_id_);

  if (!is_1w_mode_) {
    LOG_PRINT("Initializing 2W mode components...");

    channel_hopper_ = new (std::nothrow) mode2w::ChannelHopper();
    if (channel_hopper_ == nullptr) {
      LOG_PRINT("Error: Failed to allocate ChannelHopper");
      destroy_2w_components();
      return false;
    }
    channel_hopper_->begin(CHANNEL_HOP_TIME_MS);

    auth_manager_ = new (std::nothrow) mode2w::AuthenticationManager();
    if (auth_manager_ == nullptr) {
      LOG_PRINT("Error: Failed to allocate AuthenticationManager");
      destroy_2w_components();
      return false;
    }
    auth_manager_->begin(system_key_);

    beacon_handler_ = new (std::nothrow) mode2w::BeaconHandler();
    if (beacon_handler_ == nullptr) {
      LOG_PRINT("Error: Failed to allocate BeaconHandler");
      destroy_2w_components();
      return false;
    }
    beacon_handler_->begin();

    LOG_PRINT("2W mode components initialized");
  }

  // Restore the persisted rolling code and reserve a fresh block, so a reboot
  // never reuses a counter value a receiver has already seen.
  if (rolling_code_store_ != nullptr) {
    uint16_t stored = 0;
    if (rolling_code_store_->load(own_node_id_, stored)) {
      LOG_PRINTF("  Rolling code restored: %u\n", static_cast<unsigned>(stored));
    } else {
      LOG_PRINT("  Rolling code not found, starting at 0");
    }
    rolling_code_ = stored;
    rolling_code_reserved_until_ = static_cast<uint16_t>(stored + rolling_code_reserve_block_);
    rolling_code_store_->save(own_node_id_, rolling_code_reserved_until_);
  }

  if (!crypto::has_secure_random()) {
    LOG_PRINT("Warning: no secure random source - 2W pairing is disabled");
  }

  initialized_ = true;
  return true;
}

int16_t IoHomeControl::configure_radio(float frequency) {
  if (radio_ == nullptr) {
    LOG_PRINT("Error: Radio not initialized");
    return RADIOLIB_ERR_CHIP_NOT_FOUND;
  }

  LOG_PRINTF("Configuring radio on %.2f MHz\n", frequency);

  int16_t state = radio_->setFrequency(frequency);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setFrequency failed (%d)\n", state);
    return state;
  }

  // Start at the highest power and step down until the module accepts a value.
  int8_t power = 20;
  do {
    state = radio_->setOutputPower(power);
  } while (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER && --power >= -3);

  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setOutputPower failed (%d)\n", state);
    return state;
  }

  // RadioLib's FSK data rate is expressed in kbps / kHz.
  DataRate_t data_rate;
  data_rate.fsk.bitRate = BIT_RATE;        // 38.4 kbps
  data_rate.fsk.freqDev = FREQ_DEVIATION;  // 19.2 kHz

  state = radio_->setDataRate(data_rate);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setDataRate failed (%d)\n", state);
    return state;
  }

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

  // Use the pre-bitswapped sync word. Deriving it from SYNC_WORD by shifting
  // produces {0x00, 0xFF, 0x33}, which no io-homecontrol device will match.
  uint8_t sync_word[SYNC_WORD_LEN];
  memcpy(sync_word, SYNC_WORD_BYTES, SYNC_WORD_LEN);

  state = radio_->setSyncWord(sync_word, SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setSyncWord failed (%d)\n", state);
    return state;
  }

  state = radio_->setPreambleLength(PREAMBLE_LENGTH);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: setPreambleLength failed (%d)\n", state);
    return state;
  }

  LOG_PRINT("Radio configured successfully");
  return RADIOLIB_ERR_NONE;
}

void IoHomeControl::notify_packet_received() {
  g_packet_flag = true;
}

int16_t IoHomeControl::start_receive(FrameReceivedCallback callback) {
  if (!initialized_ || radio_ == nullptr) {
    LOG_PRINT("Error: Not initialized");
    return RADIOLIB_ERR_CHIP_NOT_FOUND;
  }

  rx_callback_ = callback;

  radio_->setPacketReceivedAction(packet_isr);
  g_packet_flag = false;

  const int16_t state = radio_->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: startReceive failed (%d)\n", state);
    radio_->clearPacketReceivedAction();
    return state;
  }

  receiving_ = true;
  LOG_PRINT("Receiving started");
  return RADIOLIB_ERR_NONE;
}

void IoHomeControl::stop_receive() {
  if (!receiving_ || radio_ == nullptr) {
    return;
  }

  radio_->clearPacketReceivedAction();
  radio_->standby();
  receiving_ = false;
  g_packet_flag = false;
  LOG_PRINT("Receiving stopped");
}

void IoHomeControl::reset_rx_stats() {
  rx_stats_ = RxStats{};
  last_reject_ = RxReject::NONE;
}

RxReject IoHomeControl::screen_frame(const frame::IoFrame* frame) {
  // CRC is checked without a key so malformed traffic is dropped cheaply.
  if (!frame::validate_frame(frame)) {
    return RxReject::CRC;
  }

  if (!frame->authenticated) {
    // Bootstrap commands legitimately carry no MAC: the peers have no shared
    // key yet. Everything else must be authenticated.
    if (accept_plain_frames_ || frame::is_unauthenticated_command(frame->command_id)) {
      return RxReject::NONE;
    }
    return RxReject::UNAUTHENTICATED;
  }

  const uint8_t* challenge = nullptr;
  if (!frame->is_1w_mode) {
    if (frame->command_id == CMD_CHALLENGE_REQUEST && frame->data_len >= HMAC_SIZE) {
      // A peer opening a handshake carries its challenge in the payload, and
      // the MAC is computed over exactly that. Verifying against our own
      // nonce would reject every peer-initiated handshake.
      //
      // This proves the sender holds the key, not that the frame is fresh -
      // the freshness of the session comes from the nonce we generate in the
      // response we send back.
      challenge = frame->data;
    } else if (auth_manager_ != nullptr && auth_manager_->has_active_challenge(NOW_MS())) {
      challenge = auth_manager_->get_current_challenge();
    } else {
      // No nonce to verify against. Do not fall back to the zeroed buffer,
      // which would check the MAC against an attacker-guessable value.
      return RxReject::MAC;
    }
  }

  if (!frame::validate_frame(frame, system_key_, challenge)) {
    return RxReject::MAC;
  }

  // A valid MAC proves authorship, not freshness: without this check a
  // recorded frame could simply be replayed off the air.
  if (frame->is_1w_mode) {
    if (!replay_guard_.accept(frame->src_node, frame::get_rolling_code(frame))) {
      return RxReject::REPLAY;
    }
  } else {
    // 2W frames have no sequence number - freshness comes from the session
    // challenge, which signs several frames. Reject a MAC this node produced
    // recently, which is either a protocol repeat we should act on once or a
    // replay.
    if (!replay_guard_.accept_mac(frame->src_node, frame->hmac)) {
      return RxReject::REPLAY;
    }
  }

  return RxReject::NONE;
}

bool IoHomeControl::check_received(frame::IoFrame* frame, int16_t* rssi, float* snr) {
  if (!receiving_ || frame == nullptr || radio_ == nullptr) {
    return false;
  }

  if (!g_packet_flag) {
    return false;
  }
  g_packet_flag = false;

  const size_t len = radio_->getPacketLength();
  uint8_t buffer[FRAME_MAX_SIZE];

  if (len == 0 || len > sizeof(buffer)) {
    // Nothing usable; hand the radio back to receive mode.
    radio_->startReceive();
    if (len > sizeof(buffer)) {
      rx_stats_.malformed++;
      last_reject_ = RxReject::MALFORMED;
    }
    return false;
  }

  const int16_t state = radio_->readData(buffer, len);

  // Capture the link metrics before restarting reception.
  const int16_t rssi_val = radio_->getRSSI();
  const float snr_val = radio_->getSNR();

  radio_->startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: readData failed (%d)\n", state);
    rx_stats_.radio_errors++;
    last_reject_ = RxReject::RADIO_ERROR;
    return false;
  }

  if (!frame::parse_frame(buffer, len, frame)) {
    rx_stats_.malformed++;
    last_reject_ = RxReject::MALFORMED;
    return false;
  }

  const RxReject reject = screen_frame(frame);
  last_reject_ = reject;

  switch (reject) {
    case RxReject::NONE:
      break;
    case RxReject::CRC:
      rx_stats_.crc_failures++;
      LOG_PRINT("Frame dropped: CRC mismatch");
      return false;
    case RxReject::MAC:
      rx_stats_.mac_failures++;
      LOG_PRINT("Frame dropped: MAC verification failed");
      return false;
    case RxReject::REPLAY:
      rx_stats_.replays++;
      LOG_PRINT("Frame dropped: replayed rolling code");
      return false;
    case RxReject::UNAUTHENTICATED:
      rx_stats_.unauthenticated++;
      LOG_PRINT("Frame dropped: authentication required");
      return false;
    default:
      return false;
  }

  rx_stats_.accepted++;

  if (rssi != nullptr) {
    *rssi = rssi_val;
  }
  if (snr != nullptr) {
    *snr = snr_val;
  }

  process_received_frame(frame, rssi_val, snr_val);

  if (rx_callback_ != nullptr) {
    rx_callback_(frame, rssi_val, snr_val);
  }

  return true;
}

uint16_t IoHomeControl::consume_rolling_code() {
  const uint16_t code = rolling_code_;
  rolling_code_ = static_cast<uint16_t>(rolling_code_ + 1);

  if (rolling_code_store_ == nullptr) {
    return code;
  }

  // Only touch flash when the reserved block runs out. Writing on every
  // command would wear out the NVS partition within weeks of normal use.
  // `remaining` is modular, so it stays correct across the 16-bit wrap; a
  // value larger than the block size means the counter overshot the
  // reservation and a fresh one is due.
  const uint16_t remaining = static_cast<uint16_t>(rolling_code_reserved_until_ - rolling_code_);

  if (remaining == 0 || remaining > rolling_code_reserve_block_) {
    rolling_code_reserved_until_ =
      static_cast<uint16_t>(rolling_code_ + rolling_code_reserve_block_);
    rolling_code_store_->save(own_node_id_, rolling_code_reserved_until_);
  }

  return code;
}

void IoHomeControl::set_rolling_code(uint16_t code) {
  rolling_code_ = code;
  if (rolling_code_store_ != nullptr) {
    rolling_code_reserved_until_ = static_cast<uint16_t>(code + rolling_code_reserve_block_);
    rolling_code_store_->save(own_node_id_, rolling_code_reserved_until_);
  }
}

void IoHomeControl::set_rolling_code_store(RollingCodeStore* store, uint16_t reserve_block) {
  rolling_code_store_ = store;
  rolling_code_reserve_block_ = (reserve_block == 0) ? 1 : reserve_block;
}

void IoHomeControl::set_packet_length_callback(PacketLengthCallback callback, void* context) {
  packet_length_callback_ = callback;
  packet_length_context_ = context;
}

bool IoHomeControl::set_acei(uint8_t acei) {
  if (!is_acei_valid(acei)) {
    LOG_PRINT("Error: ACEI bit 0 must be set or actuators reject the frame");
    return false;
  }
  acei_ = acei;
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

  if (dest_node == nullptr) {
    LOG_PRINT("Error: dest_node is null");
    return false;
  }

  frame::IoFrame tx_frame;
  frame::init_frame(&tx_frame, is_1w_mode_);
  frame::set_destination(&tx_frame, dest_node);
  frame::set_source(&tx_frame, own_node_id_);

  if (!frame::set_command(&tx_frame, cmd_id, params, params_len)) {
    LOG_PRINT("Error: set_command failed");
    return false;
  }

  if (is_1w_mode_) {
    frame::set_rolling_code(&tx_frame, consume_rolling_code());

    if (!frame::finalize_frame(&tx_frame, system_key_)) {
      LOG_PRINT("Error: finalize_frame failed");
      return false;
    }
  } else {
    if (auth_manager_ == nullptr) {
      LOG_PRINT("Error: 2W authentication manager not initialized");
      return false;
    }

    // The 2W MAC is bound to the challenge from the current handshake. It
    // stays usable once the peer has answered, so commands can follow the
    // handshake without renegotiating a nonce for each one.
    if (!auth_manager_->has_active_challenge(NOW_MS())) {
      LOG_PRINT("Error: no active challenge - run the 2W handshake first");
      return false;
    }

    if (!frame::finalize_frame(&tx_frame, system_key_, auth_manager_->get_current_challenge())) {
      LOG_PRINT("Error: finalize_frame failed");
      return false;
    }
  }

  return transmit_frame(&tx_frame);
}

bool IoHomeControl::send_execute(
  const uint8_t dest_node[NODE_ID_SIZE],
  uint16_t main_param,
  uint8_t fp1,
  uint8_t fp2
) {
  // Command 0x00 payload: originator | ACEI | main parameter | FP1 | FP2
  const uint8_t params[EXECUTE_PAYLOAD_SIZE] = {
    static_cast<uint8_t>(originator_),
    acei_,
    static_cast<uint8_t>((main_param >> 8) & 0xFF),
    static_cast<uint8_t>(main_param & 0xFF),
    fp1,
    fp2
  };

  return send_command(dest_node, CMD_EXECUTE, params, sizeof(params));
}

bool IoHomeControl::set_position(const uint8_t dest_node[NODE_ID_SIZE], uint8_t percent_open) {
  if (percent_open > 100) {
    percent_open = 100;
  }

  LOG_PRINTF("Setting position to %u%% open\n", static_cast<unsigned>(percent_open));

  // The wire value counts closure: 0x0000 is fully open, 0xC800 fully closed.
  const uint16_t main_param = mp_from_percent_closed(static_cast<uint8_t>(100 - percent_open));
  return send_execute(dest_node, main_param);
}

bool IoHomeControl::open(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_PRINT("Opening actuator");
  return send_execute(dest_node, MP_OPEN);
}

bool IoHomeControl::close(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_PRINT("Closing actuator");
  return send_execute(dest_node, MP_CLOSE);
}

bool IoHomeControl::stop(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_PRINT("Stopping actuator");
  return send_execute(dest_node, MP_STOP);
}

int16_t IoHomeControl::get_rssi() {
  return (radio_ != nullptr) ? radio_->getRSSI() : 0;
}

float IoHomeControl::get_snr() {
  return (radio_ != nullptr) ? radio_->getSNR() : 0.0f;
}

bool IoHomeControl::transmit_frame(const frame::IoFrame* frame) {
  if (frame == nullptr || radio_ == nullptr) {
    return false;
  }

  uint8_t buffer[FRAME_MAX_SIZE];
  const size_t len = frame::serialize_frame(frame, buffer, sizeof(buffer));

  if (len == 0) {
    LOG_PRINT("Error: serialize_frame failed");
    return false;
  }

  if (verbose_) {
    LOG_PRINTF("Transmitting %u bytes:\n", static_cast<unsigned>(len));
    for (size_t i = 0; i < len; i++) {
      LOG_PRINTF("%02X ", buffer[i]);
    }
    LOG_PRINT("");
  }

  const bool was_receiving = receiving_;
  if (was_receiving) {
    stop_receive();
  }

  // In fixed-length FSK mode the radio sends exactly the programmed number of
  // bytes, so narrow it to this frame and widen it again for reception.
  if (packet_length_callback_ != nullptr) {
    const int16_t length_state =
      packet_length_callback_(static_cast<uint8_t>(len), packet_length_context_);
    if (length_state != RADIOLIB_ERR_NONE) {
      LOG_PRINTF("Warning: packet length hook failed (%d)\n", length_state);
    }
  }

  const int16_t state = radio_->transmit(buffer, len);

  if (packet_length_callback_ != nullptr) {
    packet_length_callback_(FRAME_MAX_SIZE, packet_length_context_);
  }

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
  if (message != nullptr) {
    LOG_PRINT(message);
  }
}

void IoHomeControl::process_received_frame(const frame::IoFrame* frame, int16_t rssi, float snr) {
  const unsigned long now = NOW_MS();

  if (!is_1w_mode_ && beacon_handler_ != nullptr) {
    if (beacon_handler_->process_beacon(frame, rssi, snr, now)) {
      LOG_PRINT("Beacon received");
    }
  }

  if (discovery_manager_ != nullptr) {
    discovery_manager_->process_discovery_response(frame, rssi, now);
  }

  if (!is_1w_mode_ && auth_manager_ != nullptr && frame->command_id == CMD_CHALLENGE_RESPONSE) {
    if (auth_manager_->verify_challenge_response(frame, now)) {
      LOG_PRINT("Authentication successful");
    } else {
      LOG_PRINT("Authentication failed");
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

  channel_hopper_->reset(NOW_US());
  channel_hopper_->set_enabled(enable);
  LOG_PRINTF("Frequency hopping %s\n", enable ? "enabled" : "disabled");
  return true;
}

bool IoHomeControl::update_frequency_hopping() {
  if (is_1w_mode_ || channel_hopper_ == nullptr || !channel_hopper_->is_enabled() ||
      radio_ == nullptr) {
    return false;
  }

  // The 2.7 ms dwell time needs microsecond timing; millis() cannot express it.
  if (!channel_hopper_->update_us(NOW_US())) {
    return false;
  }

  const float new_freq = channel_hopper_->get_current_frequency();
  const int16_t state = radio_->setFrequency(new_freq);

  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("Error: Failed to switch channel (%d)\n", state);
    return false;
  }

  LOG_PRINTF("Switched to channel: %.2f MHz\n", new_freq);
  return true;
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

  if (auth_manager_ == nullptr || dest_node == nullptr) {
    LOG_PRINT("Error: Authentication manager not initialized");
    return false;
  }

  frame::IoFrame tx_frame;
  if (!auth_manager_->create_challenge_request(&tx_frame, dest_node, own_node_id_, NOW_MS())) {
    LOG_PRINT("Error: Failed to create challenge request");
    return false;
  }

  LOG_PRINT("Sending challenge request");
  return transmit_frame(&tx_frame);
}

bool IoHomeControl::send_challenge_response(const uint8_t dest_node[NODE_ID_SIZE],
                                            const uint8_t challenge[HMAC_SIZE]) {
  if (is_1w_mode_) {
    LOG_PRINT("Error: Challenge-response only available in 2W mode");
    return false;
  }

  if (auth_manager_ == nullptr || dest_node == nullptr || challenge == nullptr) {
    LOG_PRINT("Error: Invalid parameters");
    return false;
  }

  frame::IoFrame tx_frame;
  if (!auth_manager_->create_challenge_response(&tx_frame, dest_node, own_node_id_, challenge)) {
    LOG_PRINT("Error: Failed to create challenge response");
    return false;
  }

  LOG_PRINT("Sending challenge response");
  return transmit_frame(&tx_frame);
}

mode2w::ChallengeState IoHomeControl::get_auth_state() {
  if (auth_manager_ == nullptr) {
    return mode2w::ChallengeState::IDLE;
  }
  return auth_manager_->get_state(NOW_MS());
}

bool IoHomeControl::start_discovery(uint8_t device_type, unsigned long timeout_ms) {
  if (discovery_manager_ == nullptr) {
    LOG_PRINT("Error: Discovery manager not initialized");
    return false;
  }

  LOG_PRINTF("Starting discovery (device type: 0x%02X, timeout: %lu ms)\n",
             device_type, timeout_ms);
  discovery_manager_->start_discovery(device_type, timeout_ms, NOW_MS());

  frame::IoFrame tx_frame;
  if (!discovery_manager_->create_discovery_request(&tx_frame, device_type)) {
    LOG_PRINT("Error: Failed to create discovery request");
    discovery_manager_->stop_discovery();
    return false;
  }

  return transmit_frame(&tx_frame);
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

bool IoHomeControl::get_discovered_device(size_t index, mode2w::DiscoveredDevice* device) const {
  if (discovery_manager_ == nullptr) {
    return false;
  }
  return discovery_manager_->get_discovered_device(index, device);
}

bool IoHomeControl::pair_device_1w(const uint8_t dest_node[NODE_ID_SIZE],
                                   const uint8_t new_system_key[AES_KEY_SIZE],
                                   uint8_t manufacturer) {
  if (dest_node == nullptr || new_system_key == nullptr) {
    LOG_PRINT("Error: Invalid parameters (nullptr)");
    return false;
  }

  if (discovery_manager_ == nullptr) {
    LOG_PRINT("Error: Discovery manager not initialized");
    return false;
  }

  LOG_PRINT("Pairing device (1W mode)");

  // docs/linklayer.md "1W Discovery": remove the previous 1W key (0x39), then
  // send the encrypted key (0x30). Both frames are plain.
  frame::IoFrame tx_frame;
  if (discovery_manager_->create_remove_1w_controller(&tx_frame, dest_node, own_node_id_)) {
    transmit_frame(&tx_frame);
  }

  if (!discovery_manager_->create_key_transfer_1w(&tx_frame, dest_node, own_node_id_,
                                                  new_system_key, manufacturer, rolling_code_)) {
    LOG_PRINT("Error: Failed to create key transfer frame");
    return false;
  }

  return transmit_frame(&tx_frame);
}

bool IoHomeControl::pair_device_2w(const uint8_t dest_node[NODE_ID_SIZE],
                                   const uint8_t new_system_key[AES_KEY_SIZE]) {
  if (dest_node == nullptr || new_system_key == nullptr) {
    LOG_PRINT("Error: Invalid parameters (nullptr)");
    return false;
  }

  if (auth_manager_ == nullptr || discovery_manager_ == nullptr) {
    LOG_PRINT("Error: 2W components not initialized");
    return false;
  }

  LOG_PRINT("Pairing device (2W mode)");

  uint8_t challenge[HMAC_SIZE];
  if (!auth_manager_->generate_challenge(challenge, NOW_MS())) {
    LOG_PRINT("Error: no secure random source for the pairing challenge");
    return false;
  }

  frame::IoFrame tx_frame;
  const bool built = discovery_manager_->create_key_transfer_2w(
    &tx_frame, dest_node, own_node_id_, new_system_key, challenge);
  crypto::secure_zero(challenge, sizeof(challenge));

  if (!built) {
    LOG_PRINT("Error: Failed to create key transfer frame");
    return false;
  }

  return transmit_frame(&tx_frame);
}

bool IoHomeControl::has_recent_beacon(unsigned long timeout_ms) {
  if (beacon_handler_ == nullptr) {
    return false;
  }
  return beacon_handler_->has_recent_beacon(NOW_MS(), timeout_ms);
}

bool IoHomeControl::get_last_beacon(mode2w::BeaconInfo* info) const {
  if (beacon_handler_ == nullptr) {
    return false;
  }
  return beacon_handler_->get_last_beacon(info);
}

} // namespace iohome
