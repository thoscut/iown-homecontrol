/**
 * @file IoHomeControl.cpp
 * @brief io-homecontrol Node Controller Implementation
 * @author iown-homecontrol project
 */

#include "IoHomeControl.h"
#include <string.h>
#include <new>

#include <stdarg.h>
#include <stdio.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #define NOW_MS() millis()
  #define NOW_US() micros()
#else
  #include <time.h>
  // clock() is signed; cast before widening so the division stays well defined.
  #define NOW_MS() \
    ((unsigned long)((unsigned long long)(clock()) * 1000ULL / (unsigned long long)(CLOCKS_PER_SEC)))
  #define NOW_US() \
    ((unsigned long)((unsigned long long)(clock()) * 1000000ULL / (unsigned long long)(CLOCKS_PER_SEC)))
#endif

// Logging goes through emit_log(), which either hands the formatted message to
// the application's sink or falls back to the serial port. Severity is part of
// the call now: it used to live in the message text as an "Error:" prefix,
// which nothing could filter on.
#define LOG_ERROR(...)  emit_log(LogLevel::ERROR, __VA_ARGS__)
#define LOG_WARN(...)   emit_log(LogLevel::WARN, __VA_ARGS__)
#define LOG_INFO(...)   emit_log(LogLevel::INFO, __VA_ARGS__)
#define LOG_DEBUG(...)  emit_log(LogLevel::DEBUG, __VA_ARGS__)

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
    raw_frame_callback_(nullptr),
    raw_frame_context_(nullptr),
    log_callback_(nullptr),
    log_context_(nullptr),
    log_min_level_(LogLevel::DEBUG),
    rolling_code_store_(nullptr),
    rolling_code_reserved_until_(0),
    rolling_code_reserve_block_(64),
    last_reject_(RxReject::NONE),
    rx_stats_{},
    channel_hopper_(nullptr),
    auth_manager_(nullptr),
    beacon_handler_(nullptr),
    discovery_manager_(nullptr),
    key_received_callback_(nullptr),
    key_received_context_(nullptr),
    accept_pairing_(false),
    pairing_state_(Pairing2W::IDLE),
    pairing_started_ms_(0),
    pairing_clock_overridden_(false),
    pairing_clock_ms_(0)
{
  memset(own_node_id_, 0, NODE_ID_SIZE);
  memset(system_key_, 0, AES_KEY_SIZE);
  memset(pairing_peer_, 0, NODE_ID_SIZE);
  memset(pairing_key_, 0, AES_KEY_SIZE);
  memset(pairing_challenge_, 0, HMAC_SIZE);
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
    LOG_ERROR("Invalid parameters (nullptr)");
    return false;
  }

  if (radio_ == nullptr) {
    LOG_ERROR("Radio not set");
    return false;
  }

  memcpy(own_node_id_, own_node_id, NODE_ID_SIZE);
  memcpy(system_key_, system_key, AES_KEY_SIZE);
  is_1w_mode_ = is_1w;

  LOG_INFO("IoHomeControl: Initializing (%s mode)", is_1w ? "1W" : "2W");
  LOG_INFO("  Node ID: %02X %02X %02X",
             own_node_id_[0], own_node_id_[1], own_node_id_[2]);

  // Clean up any existing 2W components before re-initialization.
  destroy_2w_components();

  // The discovery manager is useful in both modes: 1W pairing also relies on it.
  discovery_manager_ = new (std::nothrow) mode2w::DiscoveryManager();
  if (discovery_manager_ == nullptr) {
    LOG_ERROR("Failed to allocate DiscoveryManager");
    return false;
  }
  discovery_manager_->begin(own_node_id_);

  if (!is_1w_mode_) {
    LOG_INFO("Initializing 2W mode components...");

    channel_hopper_ = new (std::nothrow) mode2w::ChannelHopper();
    if (channel_hopper_ == nullptr) {
      LOG_ERROR("Failed to allocate ChannelHopper");
      destroy_2w_components();
      return false;
    }
    channel_hopper_->begin(CHANNEL_HOP_TIME_MS);

    auth_manager_ = new (std::nothrow) mode2w::AuthenticationManager();
    if (auth_manager_ == nullptr) {
      LOG_ERROR("Failed to allocate AuthenticationManager");
      destroy_2w_components();
      return false;
    }
    auth_manager_->begin(system_key_);

    beacon_handler_ = new (std::nothrow) mode2w::BeaconHandler();
    if (beacon_handler_ == nullptr) {
      LOG_ERROR("Failed to allocate BeaconHandler");
      destroy_2w_components();
      return false;
    }
    beacon_handler_->begin();

    LOG_INFO("2W mode components initialized");
  }

  // Restore the persisted rolling code and reserve a fresh block, so a reboot
  // never reuses a counter value a receiver has already seen.
  if (rolling_code_store_ != nullptr) {
    uint16_t stored = 0;
    if (rolling_code_store_->load(own_node_id_, stored)) {
      LOG_INFO("  Rolling code restored: %u", static_cast<unsigned>(stored));
    } else {
      LOG_INFO("  Rolling code not found, starting at 0");
    }
    rolling_code_ = stored;
    rolling_code_reserved_until_ = static_cast<uint16_t>(stored + rolling_code_reserve_block_);
    rolling_code_store_->save(own_node_id_, rolling_code_reserved_until_);
  }

  if (!crypto::has_secure_random()) {
    LOG_WARN("no secure random source - 2W pairing is disabled");
  }

  initialized_ = true;
  return true;
}

int16_t IoHomeControl::configure_radio(float frequency) {
  if (radio_ == nullptr) {
    LOG_ERROR("Radio not initialized");
    return RADIOLIB_ERR_CHIP_NOT_FOUND;
  }

  LOG_INFO("Configuring radio on %.2f MHz", frequency);

  int16_t state = radio_->setFrequency(frequency);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setFrequency failed (%d)", state);
    return state;
  }

  // Start at the highest power and step down until the module accepts a value.
  int8_t power = 20;
  do {
    state = radio_->setOutputPower(power);
  } while (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER && --power >= -3);

  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setOutputPower failed (%d)", state);
    return state;
  }

  // RadioLib's FSK data rate is expressed in kbps / kHz.
  DataRate_t data_rate;
  data_rate.fsk.bitRate = BIT_RATE;        // 38.4 kbps
  data_rate.fsk.freqDev = FREQ_DEVIATION;  // 19.2 kHz

  state = radio_->setDataRate(data_rate);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setDataRate failed (%d)", state);
    return state;
  }

  state = radio_->setEncoding(RADIOLIB_ENCODING_NRZ);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setEncoding failed (%d)", state);
    return state;
  }

  state = radio_->setDataShaping(RADIOLIB_SHAPING_NONE);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setDataShaping failed (%d)", state);
    return state;
  }

  // Use the pre-bitswapped sync word. Deriving it from SYNC_WORD by shifting
  // produces {0x00, 0xFF, 0x33}, which no io-homecontrol device will match.
  uint8_t sync_word[SYNC_WORD_LEN];
  memcpy(sync_word, SYNC_WORD_BYTES, SYNC_WORD_LEN);

  state = radio_->setSyncWord(sync_word, SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setSyncWord failed (%d)", state);
    return state;
  }

  state = radio_->setPreambleLength(PREAMBLE_LENGTH);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("setPreambleLength failed (%d)", state);
    return state;
  }

  LOG_INFO("Radio configured successfully");
  return RADIOLIB_ERR_NONE;
}

void IoHomeControl::notify_packet_received() {
  g_packet_flag = true;
}

int16_t IoHomeControl::start_receive(FrameReceivedCallback callback) {
  if (!initialized_ || radio_ == nullptr) {
    LOG_ERROR("Not initialized");
    return RADIOLIB_ERR_CHIP_NOT_FOUND;
  }

  rx_callback_ = callback;

  radio_->setPacketReceivedAction(packet_isr);
  g_packet_flag = false;

  const int16_t state = radio_->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("startReceive failed (%d)", state);
    radio_->clearPacketReceivedAction();
    return state;
  }

  receiving_ = true;
  LOG_INFO("Receiving started");
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
  LOG_INFO("Receiving stopped");
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
  uint8_t wire[RX_CAPTURE_SIZE];

  if (len == 0 || len > sizeof(wire)) {
    // Nothing usable; hand the radio back to receive mode.
    radio_->startReceive();
    if (len > sizeof(wire)) {
      rx_stats_.malformed++;
      last_reject_ = RxReject::MALFORMED;
    }
    return false;
  }

  const int16_t state = radio_->readData(wire, len);

  // Capture the link metrics before restarting reception.
  const int16_t rssi_val = radio_->getRSSI();
  const float snr_val = radio_->getSNR();

  radio_->startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("readData failed (%d)", state);
    rx_stats_.radio_errors++;
    last_reject_ = RxReject::RADIO_ERROR;
    return false;
  }

  rx_stats_.received++;

  // Hand the raw on-air bytes to the sniffer before anything is stripped or
  // interpreted - a frame that fails validation is often the one worth seeing,
  // and a sniffer wants the bytes exactly as the radio delivered them, framing
  // and all.
  if (raw_frame_callback_ != nullptr) {
    raw_frame_callback_(wire, len, rssi_val, snr_val, raw_frame_context_);
  }

  // Strip the UART start/stop framing to get the frame itself. Without this the
  // "control byte" is really a start bit and part of ctrl0, the declared length
  // runs 10/8 long, and the CRC never checks out.
  uint8_t buffer[FRAME_MAX_SIZE];
  const size_t frame_len = phy::uart_decode_frame(wire, len, buffer, sizeof(buffer));
  if (frame_len == 0) {
    rx_stats_.malformed++;
    last_reject_ = RxReject::MALFORMED;
    return false;
  }

  if (!frame::parse_frame(buffer, frame_len, frame)) {
    rx_stats_.malformed++;
    last_reject_ = RxReject::MALFORMED;
    return false;
  }

  // A pairing handshake that stalled (the peer never answered) must not keep
  // diverting unrelated frames from the MAC gate. Age it out before we decide
  // how to route this frame, so a stranger's 0x3C/0x32 goes through the normal
  // gate once our own attempt has timed out.
  expire_stale_pairing();

  // Pairing frames run their own short exchange and must not go through the
  // session authentication gate below: while a key is being transferred the
  // peers do not yet share the key that gate verifies against. They are still
  // CRC-checked - a corrupt one is dropped - but not MAC-checked.
  if (is_pairing_frame(frame)) {
    if (frame::validate_frame(frame)) {
      rx_stats_.accepted++;
      last_reject_ = RxReject::NONE;
      handle_pairing_frame(frame);
      if (rssi != nullptr) { *rssi = rssi_val; }
      if (snr != nullptr) { *snr = snr_val; }
      return true;
    }
    rx_stats_.crc_failures++;
    last_reject_ = RxReject::CRC;
    return false;
  }

  const RxReject reject = screen_frame(frame);
  last_reject_ = reject;

  switch (reject) {
    case RxReject::NONE:
      break;
    case RxReject::CRC:
      rx_stats_.crc_failures++;
      LOG_DEBUG("Frame dropped: CRC mismatch");
      return false;
    case RxReject::MAC:
      rx_stats_.mac_failures++;
      LOG_DEBUG("Frame dropped: MAC verification failed");
      return false;
    case RxReject::REPLAY:
      rx_stats_.replays++;
      LOG_DEBUG("Frame dropped: replayed rolling code");
      return false;
    case RxReject::UNAUTHENTICATED:
      rx_stats_.unauthenticated++;
      LOG_DEBUG("Frame dropped: authentication required");
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

void IoHomeControl::set_raw_frame_callback(RawFrameCallback callback, void* context) {
  raw_frame_callback_ = callback;
  raw_frame_context_ = context;
}

void IoHomeControl::set_log_callback(LogCallback callback, void* context, LogLevel min_level) {
  log_callback_ = callback;
  log_context_ = context;
  log_min_level_ = min_level;
}

bool IoHomeControl::log_enabled(LogLevel level) const {
  if (log_callback_ != nullptr) {
    return static_cast<uint8_t>(level) <= static_cast<uint8_t>(log_min_level_);
  }
  return verbose_;
}

void IoHomeControl::emit_log(LogLevel level, const char* format, ...) const {
  if (format == nullptr || !log_enabled(level)) {
    return;
  }

  // Every message the library produces fits well inside this; a longer one is
  // truncated rather than dropped, because a truncated diagnostic still says
  // more than silence. No allocation: this runs on a device with 320 KB of RAM
  // and is called from paths that must not fail for want of a heap block.
  char message[192];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  if (written < 0) {
    return;  // Encoding error; there is nothing meaningful to report.
  }

  if (log_callback_ != nullptr) {
    log_callback_(level, message, log_context_);
    return;
  }

  // No sink: the built-in output, which set_verbose() governs. The severity
  // that used to be spelled into each message goes back in here, once.
  static const char* const kLevelNames[] = {"ERROR", "WARN", "INFO", "DEBUG"};
  const uint8_t index = static_cast<uint8_t>(level);
  const char* const name =
    (index < (sizeof(kLevelNames) / sizeof(kLevelNames[0]))) ? kLevelNames[index] : "?";
#ifdef ARDUINO
  Serial.print("[iohc ");
  Serial.print(name);
  Serial.print("] ");
  Serial.println(message);
#else
  printf("[iohc %s] %s\n", name, message);
#endif
}

bool IoHomeControl::set_acei(uint8_t acei) {
  if (!is_acei_valid(acei)) {
    LOG_ERROR("ACEI bit 0 must be set or actuators reject the frame");
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
    LOG_ERROR("Not initialized");
    return false;
  }

  if (dest_node == nullptr) {
    LOG_ERROR("dest_node is null");
    return false;
  }

  frame::IoFrame tx_frame;
  frame::init_frame(&tx_frame, is_1w_mode_);
  frame::set_destination(&tx_frame, dest_node);
  frame::set_source(&tx_frame, own_node_id_);

  if (!frame::set_command(&tx_frame, cmd_id, params, params_len)) {
    LOG_ERROR("set_command failed");
    return false;
  }

  if (is_1w_mode_) {
    frame::set_rolling_code(&tx_frame, consume_rolling_code());

    if (!frame::finalize_frame(&tx_frame, system_key_)) {
      LOG_ERROR("finalize_frame failed");
      return false;
    }
  } else {
    if (auth_manager_ == nullptr) {
      LOG_ERROR("2W authentication manager not initialized");
      return false;
    }

    // The 2W MAC is bound to the challenge from the current handshake. It
    // stays usable once the peer has answered, so commands can follow the
    // handshake without renegotiating a nonce for each one.
    if (!auth_manager_->has_active_challenge(NOW_MS())) {
      LOG_ERROR("no active challenge - run the 2W handshake first");
      return false;
    }

    if (!frame::finalize_frame(&tx_frame, system_key_, auth_manager_->get_current_challenge())) {
      LOG_ERROR("finalize_frame failed");
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
  const uint8_t fps[2] = {fp1, fp2};
  return send_execute_fp(dest_node, main_param, fps, sizeof(fps));
}

bool IoHomeControl::send_execute_fp(
  const uint8_t dest_node[NODE_ID_SIZE],
  uint16_t main_param,
  const uint8_t* fps,
  size_t fp_count
) {
  // FP1 and FP2 are always on the wire, so anything shorter is not an Execute
  // payload. More may follow - see EXECUTE_PAYLOAD_MIN_SIZE.
  if (fps == nullptr || fp_count < 2 || fp_count > EXECUTE_MAX_FUNCTIONAL_PARAMS) {
    LOG_ERROR("execute needs 2 to 16 functional parameters");
    return false;
  }

  // Command 0x00 payload: originator | ACEI | main parameter | FP1 | FP2 | ...
  uint8_t params[EXECUTE_PAYLOAD_PREFIX_SIZE + EXECUTE_MAX_FUNCTIONAL_PARAMS];
  params[0] = static_cast<uint8_t>(originator_);
  params[1] = acei_;
  params[2] = static_cast<uint8_t>((main_param >> 8) & 0xFF);
  params[3] = static_cast<uint8_t>(main_param & 0xFF);
  memcpy(&params[EXECUTE_PAYLOAD_PREFIX_SIZE], fps, fp_count);

  return send_command(dest_node, CMD_EXECUTE,
                      params, EXECUTE_PAYLOAD_PREFIX_SIZE + fp_count);
}

bool IoHomeControl::set_position(const uint8_t dest_node[NODE_ID_SIZE], uint8_t percent_open) {
  if (percent_open > 100) {
    percent_open = 100;
  }

  LOG_INFO("Setting position to %u%% open", static_cast<unsigned>(percent_open));

  // The wire value counts closure: 0x0000 is fully open, 0xC800 fully closed.
  const uint16_t main_param = mp_from_percent_closed(static_cast<uint8_t>(100 - percent_open));
  return send_execute(dest_node, main_param);
}

bool IoHomeControl::open(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_INFO("Opening actuator");
  return send_execute(dest_node, MP_OPEN);
}

bool IoHomeControl::close(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_INFO("Closing actuator");
  return send_execute(dest_node, MP_CLOSE);
}

bool IoHomeControl::stop(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_INFO("Stopping actuator");
  return send_execute(dest_node, MP_STOP);
}

bool IoHomeControl::ventilate(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_INFO("Secured ventilation");
  return send_execute(dest_node, MP_SECURED_VENTILATION);
}

bool IoHomeControl::force(const uint8_t dest_node[NODE_ID_SIZE]) {
  LOG_INFO("Force preset");
  return send_execute(dest_node, MP_FORCE);
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
    LOG_ERROR("serialize_frame failed");
    return false;
  }

  // One message, not one per byte: a sink receives whole messages, and the
  // old byte-at-a-time form relied on the serial port not adding line breaks.
  // FRAME_MAX_SIZE is 34, so three characters each fits comfortably.
  if (log_enabled(LogLevel::DEBUG)) {
    char hex[FRAME_MAX_SIZE * 3 + 1];
    size_t written = 0;
    for (size_t i = 0; i < len && written + 3 < sizeof(hex); i++) {
      written += static_cast<size_t>(snprintf(hex + written, sizeof(hex) - written,
                                              "%02X ", buffer[i]));
    }
    hex[written] = '\0';
    LOG_DEBUG("Transmitting %u bytes: %s", static_cast<unsigned>(len), hex);
  }

  // Wrap the frame in the on-air UART framing every io-homecontrol node
  // expects: each byte becomes a start bit, its eight data bits
  // least-significant first, and a stop bit. A plain FSK radio does not add
  // this, so it has to be done here; a device reading our un-framed bytes would
  // see a garbage length and a failing CRC and drop the frame. See
  // protocol/iohome_phy_framing.h.
  uint8_t wire[RX_CAPTURE_SIZE];
  const size_t wire_len = phy::uart_encode(buffer, len, wire, sizeof(wire));
  if (wire_len == 0) {
    LOG_ERROR("uart_encode failed");
    return false;
  }

  const bool was_receiving = receiving_;
  if (was_receiving) {
    stop_receive();
  }

  // In fixed-length FSK mode the radio sends exactly the programmed number of
  // bytes, so narrow it to this frame's wire length and widen it again for
  // reception.
  if (packet_length_callback_ != nullptr) {
    const int16_t length_state =
      packet_length_callback_(static_cast<uint8_t>(wire_len), packet_length_context_);
    if (length_state != RADIOLIB_ERR_NONE) {
      LOG_WARN("packet length hook failed (%d)", length_state);
    }
  }

  const int16_t state = radio_->transmit(wire, wire_len);

  if (packet_length_callback_ != nullptr) {
    packet_length_callback_(RX_CAPTURE_SIZE, packet_length_context_);
  }

  if (was_receiving) {
    start_receive(rx_callback_);
  }

  if (state != RADIOLIB_ERR_NONE) {
    LOG_ERROR("transmit failed (%d)", state);
    return false;
  }

  LOG_INFO("Frame transmitted successfully");
  return true;
}

void IoHomeControl::log(const char* message) {
  if (message != nullptr) {
    // "%s", not the caller's string as the format: a message containing a
    // percent sign would otherwise read arguments that were never passed.
    LOG_INFO("%s", message);
  }
}

void IoHomeControl::process_received_frame(const frame::IoFrame* frame, int16_t rssi, float snr) {
  const unsigned long now = NOW_MS();

  if (!is_1w_mode_ && beacon_handler_ != nullptr) {
    if (beacon_handler_->process_beacon(frame, rssi, snr, now)) {
      LOG_INFO("Beacon received");
    }
  }

  if (discovery_manager_ != nullptr) {
    discovery_manager_->process_discovery_response(frame, rssi, now);
  }

  if (!is_1w_mode_ && auth_manager_ != nullptr && frame->command_id == CMD_CHALLENGE_RESPONSE) {
    if (auth_manager_->verify_challenge_response(frame, now)) {
      LOG_INFO("Authentication successful");
    } else {
      LOG_INFO("Authentication failed");
    }
  }
}

// ============================================================================
// 2W Mode Features Implementation
// ============================================================================

bool IoHomeControl::enable_frequency_hopping(bool enable) {
  if (is_1w_mode_) {
    LOG_WARN("Frequency hopping only available in 2W mode");
    return false;
  }

  if (channel_hopper_ == nullptr) {
    LOG_ERROR("Channel hopper not initialized");
    return false;
  }

  channel_hopper_->reset(NOW_US());
  channel_hopper_->set_enabled(enable);
  LOG_INFO("Frequency hopping %s", enable ? "enabled" : "disabled");
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
    LOG_ERROR("Failed to switch channel (%d)", state);
    return false;
  }

  LOG_INFO("Switched to channel: %.2f MHz", new_freq);
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
    LOG_ERROR("Challenge-response only available in 2W mode");
    return false;
  }

  if (auth_manager_ == nullptr || dest_node == nullptr) {
    LOG_ERROR("Authentication manager not initialized");
    return false;
  }

  frame::IoFrame tx_frame;
  if (!auth_manager_->create_challenge_request(&tx_frame, dest_node, own_node_id_, NOW_MS())) {
    LOG_ERROR("Failed to create challenge request");
    return false;
  }

  LOG_INFO("Sending challenge request");
  return transmit_frame(&tx_frame);
}

bool IoHomeControl::send_challenge_response(const uint8_t dest_node[NODE_ID_SIZE],
                                            const uint8_t challenge[HMAC_SIZE]) {
  if (is_1w_mode_) {
    LOG_ERROR("Challenge-response only available in 2W mode");
    return false;
  }

  if (auth_manager_ == nullptr || dest_node == nullptr || challenge == nullptr) {
    LOG_ERROR("Invalid parameters");
    return false;
  }

  frame::IoFrame tx_frame;
  if (!auth_manager_->create_challenge_response(&tx_frame, dest_node, own_node_id_, challenge)) {
    LOG_ERROR("Failed to create challenge response");
    return false;
  }

  LOG_INFO("Sending challenge response");
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
    LOG_ERROR("Discovery manager not initialized");
    return false;
  }

  LOG_INFO("Starting discovery (device type: 0x%02X, timeout: %lu ms)",
             device_type, timeout_ms);
  discovery_manager_->start_discovery(device_type, timeout_ms, NOW_MS());

  frame::IoFrame tx_frame;
  if (!discovery_manager_->create_discovery_request(&tx_frame, device_type)) {
    LOG_ERROR("Failed to create discovery request");
    discovery_manager_->stop_discovery();
    return false;
  }

  return transmit_frame(&tx_frame);
}

void IoHomeControl::stop_discovery() {
  if (discovery_manager_ != nullptr) {
    discovery_manager_->stop_discovery();
    LOG_INFO("Discovery stopped");
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
    LOG_ERROR("Invalid parameters (nullptr)");
    return false;
  }

  if (discovery_manager_ == nullptr) {
    LOG_ERROR("Discovery manager not initialized");
    return false;
  }

  LOG_INFO("Pairing device (1W mode)");

  // docs/linklayer.md "1W Discovery": remove the previous 1W key (0x39), then
  // send the encrypted key (0x30). Both frames are plain.
  frame::IoFrame tx_frame;
  if (discovery_manager_->create_remove_1w_controller(&tx_frame, dest_node, own_node_id_)) {
    transmit_frame(&tx_frame);
  }

  if (!discovery_manager_->create_key_transfer_1w(&tx_frame, dest_node, own_node_id_,
                                                  new_system_key, manufacturer, rolling_code_)) {
    LOG_ERROR("Failed to create key transfer frame");
    return false;
  }

  return transmit_frame(&tx_frame);
}

void IoHomeControl::set_accept_pairing(bool enabled) {
  accept_pairing_ = enabled;
}

void IoHomeControl::set_key_received_callback(KeyReceivedCallback callback, void* context) {
  key_received_callback_ = callback;
  key_received_context_ = context;
}

void IoHomeControl::set_system_key(const uint8_t key[AES_KEY_SIZE]) {
  if (key == nullptr) {
    return;
  }
  memcpy(system_key_, key, AES_KEY_SIZE);
  if (auth_manager_ != nullptr) {
    auth_manager_->begin(key);
  }
}

bool IoHomeControl::pair_device_2w(const uint8_t dest_node[NODE_ID_SIZE],
                                   const uint8_t new_system_key[AES_KEY_SIZE]) {
  if (dest_node == nullptr || new_system_key == nullptr) {
    LOG_ERROR("Invalid parameters (nullptr)");
    return false;
  }

  if (auth_manager_ == nullptr || discovery_manager_ == nullptr) {
    LOG_ERROR("2W components not initialized");
    return false;
  }

  LOG_INFO("Pairing device (2W push): asking for a challenge");

  // Step 1 of the push: ask the device for a challenge (0x31, no parameters,
  // plain). The device answers with 0x3C, and handle_pairing_frame() builds the
  // 0x32 key transfer from the nonce it chose - so the mask is bound to a
  // challenge the device actually holds. That is the piece the one-shot version
  // was missing.
  frame::IoFrame ask;
  frame::init_frame(&ask, false);  // 2W
  frame::set_destination(&ask, dest_node);
  frame::set_source(&ask, own_node_id_);
  if (!frame::set_command(&ask, CMD_ASK_CHALLENGE, nullptr, 0) ||
      !frame::finalize_frame_plain(&ask)) {
    LOG_ERROR("Failed to build the ask-challenge frame");
    return false;
  }

  memcpy(pairing_peer_, dest_node, NODE_ID_SIZE);
  memcpy(pairing_key_, new_system_key, AES_KEY_SIZE);
  pairing_state_ = Pairing2W::PUSH_WAIT_CHALLENGE;
  pairing_started_ms_ = pairing_now_ms();

  if (!transmit_frame(&ask)) {
    pairing_state_ = Pairing2W::IDLE;
    crypto::secure_zero(pairing_key_, sizeof(pairing_key_));
    return false;
  }
  return true;
}

bool IoHomeControl::pull_device_key_2w(const uint8_t dest_node[NODE_ID_SIZE]) {
  if (dest_node == nullptr) {
    LOG_ERROR("Invalid parameters (nullptr)");
    return false;
  }
  if (auth_manager_ == nullptr || discovery_manager_ == nullptr) {
    LOG_ERROR("2W components not initialized");
    return false;
  }

  // The 0x38 carries a challenge the device masks its key against. We keep it to
  // unmask the 0x32 that comes back.
  if (!crypto::random_bytes(pairing_challenge_, HMAC_SIZE)) {
    LOG_ERROR("no secure random source for the pull challenge");
    return false;
  }

  LOG_INFO("Pulling device key (2W): launching key transfer");

  frame::IoFrame launch;
  frame::init_frame(&launch, false);  // 2W
  frame::set_destination(&launch, dest_node);
  frame::set_source(&launch, own_node_id_);
  if (!frame::set_command(&launch, CMD_LAUNCH_KEY_TRANSFER, pairing_challenge_, HMAC_SIZE) ||
      !frame::finalize_frame_plain(&launch)) {
    LOG_ERROR("Failed to build the launch-key-transfer frame");
    return false;
  }

  memcpy(pairing_peer_, dest_node, NODE_ID_SIZE);
  pairing_state_ = Pairing2W::PULL_WAIT_KEY;
  pairing_started_ms_ = pairing_now_ms();

  if (!transmit_frame(&launch)) {
    pairing_state_ = Pairing2W::IDLE;
    crypto::secure_zero(pairing_challenge_, sizeof(pairing_challenge_));
    return false;
  }
  return true;
}

bool IoHomeControl::is_pairing_frame(const frame::IoFrame* frame) const {
  // The 2W pairing exchange only exists in 2W mode: it builds challenges and
  // recovers masked keys through auth_manager_/discovery_manager_, which are
  // only allocated when begin() runs in 2W mode. Diverting a frame here in 1W
  // mode would reach a null manager. A 1W node with accept_pairing_ set must
  // therefore never route an incoming 0x31/0x38 into the pairing machine - an
  // attacker could otherwise crash it with a single broadcast frame.
  if (is_1w_mode_ || auth_manager_ == nullptr) {
    return false;
  }

  // Addressed to us (or broadcast), and a command that belongs to a pairing
  // exchange this node is currently part of. 0x3C and 0x33 are only diverted
  // while we are the initiator waiting for them; otherwise 0x3C is an ordinary
  // session challenge and must go through the normal gate.
  const bool for_us = memcmp(frame->dest_node, own_node_id_, NODE_ID_SIZE) == 0 ||
                      frame::is_broadcast(frame->dest_node);
  if (!for_us) {
    return false;
  }
  switch (frame->command_id) {
    case CMD_ASK_CHALLENGE:      // 0x31 - a controller asking us to be pushed to
    case CMD_LAUNCH_KEY_TRANSFER:  // 0x38 - a controller pulling our key
      return accept_pairing_;
    case CMD_KEY_TRANSFER:   // 0x32 - pushed to us, or pulled by us
      return accept_pairing_ || pairing_state_ == Pairing2W::PULL_WAIT_KEY;
    case CMD_CHALLENGE_REQUEST:  // 0x3C
      return pairing_state_ == Pairing2W::PUSH_WAIT_CHALLENGE;
    case CMD_KEY_TRANSFER_ACK:   // 0x33
      return pairing_state_ == Pairing2W::PUSH_WAIT_ACK;
    default:
      return false;
  }
}

uint32_t IoHomeControl::pairing_now_ms() const {
  return pairing_clock_overridden_ ? pairing_clock_ms_ : static_cast<uint32_t>(NOW_MS());
}

void IoHomeControl::expire_stale_pairing() {
  if (pairing_state_ == Pairing2W::IDLE) {
    return;
  }
  // 32-bit unsigned subtraction is wrap-safe across the millis() rollover (which
  // is itself 32-bit). elapsed = now - start is correct even at the wrap.
  const uint32_t elapsed = pairing_now_ms() - pairing_started_ms_;
  if (elapsed < PAIRING_TIMEOUT_MS) {
    return;
  }
  LOG_WARN("Pairing timed out; abandoning the handshake");
  pairing_state_ = Pairing2W::IDLE;
  crypto::secure_zero(pairing_key_, sizeof(pairing_key_));
  crypto::secure_zero(pairing_challenge_, sizeof(pairing_challenge_));
}

void IoHomeControl::handle_pairing_frame(const frame::IoFrame* frame) {
  // Defence in depth: is_pairing_frame() already refuses to divert here in 1W
  // mode, but the managers this function drives are only non-null in 2W mode.
  if (is_1w_mode_ || auth_manager_ == nullptr || discovery_manager_ == nullptr) {
    return;
  }

  const bool from_peer = memcmp(frame->src_node, pairing_peer_, NODE_ID_SIZE) == 0;

  // ---- Initiator (controller pushing its key) ----------------------------
  if (pairing_state_ == Pairing2W::PUSH_WAIT_CHALLENGE &&
      frame->command_id == CMD_CHALLENGE_REQUEST && from_peer) {
    if (frame->data_len < HMAC_SIZE) {
      LOG_WARN("Pairing: challenge too short");
      return;
    }
    // Build the 0x32 with the nonce the device just chose. The IV is seeded by
    // the ask-challenge command, matching what the device will use to unmask.
    const uint8_t request_frame[1] = {CMD_ASK_CHALLENGE};
    frame::IoFrame kt;
    const bool built = discovery_manager_->create_key_transfer_2w(
      &kt, pairing_peer_, own_node_id_, pairing_key_, frame->data,
      request_frame, sizeof(request_frame));
    if (!built) {
      LOG_ERROR("Pairing: failed to build key transfer");
      pairing_state_ = Pairing2W::IDLE;
      return;
    }
    // Do NOT adopt the key yet. If this transmit fails or the 0x32 is lost, the
    // device never stores the key; adopting it here would leave the controller on
    // a key the device does not have, so every later command would fail its MAC
    // with no signal that pairing went wrong. Stay on the old key until the
    // device acknowledges (the 0x33 branch below), so a retry recovers cleanly.
    LOG_INFO("Pairing: sending key transfer");
    if (!transmit_frame(&kt)) {
      LOG_ERROR("Pairing: key transfer failed to send");
      pairing_state_ = Pairing2W::IDLE;
      crypto::secure_zero(pairing_key_, sizeof(pairing_key_));
      return;
    }
    pairing_state_ = Pairing2W::PUSH_WAIT_ACK;
    pairing_started_ms_ = pairing_now_ms();
    return;
  }

  if (pairing_state_ == Pairing2W::PUSH_WAIT_ACK &&
      frame->command_id == CMD_KEY_TRANSFER_ACK && from_peer) {
    // The device stored the key and acknowledged. Only now is it safe to speak
    // under it - this is what makes PUSH_WAIT_ACK meaningful.
    LOG_INFO("Pairing complete: device acknowledged the key");
    set_system_key(pairing_key_);
    pairing_state_ = Pairing2W::IDLE;
    crypto::secure_zero(pairing_key_, sizeof(pairing_key_));
    return;
  }

  // ---- Collector (controller pulling a device's key) ---------------------
  if (pairing_state_ == Pairing2W::PULL_WAIT_KEY &&
      frame->command_id == CMD_KEY_TRANSFER && from_peer) {
    // The device masked its key against the 0x38 we sent - its command byte
    // followed by the challenge - and that same challenge.
    uint8_t request[1 + HMAC_SIZE] = {CMD_LAUNCH_KEY_TRANSFER};
    memcpy(&request[1], pairing_challenge_, HMAC_SIZE);

    uint8_t recovered[AES_KEY_SIZE];
    const bool ok = frame->data_len >= AES_KEY_SIZE &&
                    crypto::decrypt_2w_key(frame->data, request, sizeof(request),
                                           pairing_challenge_, recovered);
    pairing_state_ = Pairing2W::IDLE;
    crypto::secure_zero(pairing_challenge_, sizeof(pairing_challenge_));

    if (!ok) {
      LOG_WARN("Pull: could not recover the device's key");
      return;
    }
    // Not adopted: this is the device's key, surfaced for per-device storage.
    LOG_INFO("Pull: recovered a device key");
    if (key_received_callback_ != nullptr) {
      key_received_callback_(recovered, frame->src_node, key_received_context_);
    }
    crypto::secure_zero(recovered, sizeof(recovered));
    return;
  }

  // ---- Follower (device accepting a pushed key, or answering a pull) ------
  if (!accept_pairing_) {
    return;
  }

  if (frame->command_id == CMD_LAUNCH_KEY_TRANSFER) {
    // A controller wants our key. Mask it against the challenge the 0x38 carried
    // and reply with a 0x32. The controller unmasks it with the same challenge.
    if (frame->data_len < HMAC_SIZE) {
      LOG_WARN("Pull request without a challenge");
      return;
    }
    uint8_t request[1 + HMAC_SIZE] = {CMD_LAUNCH_KEY_TRANSFER};
    memcpy(&request[1], frame->data, HMAC_SIZE);

    frame::IoFrame kt;
    const bool built = discovery_manager_->create_key_transfer_2w(
      &kt, frame->src_node, own_node_id_, system_key_, frame->data,
      request, sizeof(request));
    if (built) {
      LOG_INFO("Pull: sending our key");
      transmit_frame(&kt);
    } else {
      LOG_ERROR("Pull: failed to build key transfer");
    }
    return;
  }

  if (frame->command_id == CMD_ASK_CHALLENGE) {
    // Answer with a fresh challenge of our own (0x3C). create_challenge_request
    // stores the nonce, which recover_2w_key() will need to unmask the 0x32.
    memcpy(pairing_peer_, frame->src_node, NODE_ID_SIZE);
    frame::IoFrame chal;
    if (auth_manager_->create_challenge_request(&chal, frame->src_node, own_node_id_,
                                                NOW_MS())) {
      LOG_INFO("Pairing: answering with a challenge");
      transmit_frame(&chal);
    } else {
      LOG_ERROR("Pairing: could not create a challenge (no secure random?)");
    }
    return;
  }

  if (frame->command_id == CMD_KEY_TRANSFER && from_peer) {
    const uint8_t request_frame[1] = {CMD_ASK_CHALLENGE};
    uint8_t recovered[AES_KEY_SIZE];
    if (!auth_manager_->recover_2w_key(frame, request_frame, sizeof(request_frame),
                                       recovered)) {
      LOG_WARN("Pairing: could not recover the pushed key");
      return;
    }

    set_system_key(recovered);

    // Acknowledge with 0x33 so the controller knows the key landed.
    frame::IoFrame ack;
    frame::init_frame(&ack, false);
    frame::set_destination(&ack, frame->src_node);
    frame::set_source(&ack, own_node_id_);
    if (frame::set_command(&ack, CMD_KEY_TRANSFER_ACK, nullptr, 0) &&
        frame::finalize_frame_plain(&ack)) {
      transmit_frame(&ack);
    }

    LOG_INFO("Pairing: adopted a pushed system key");
    if (key_received_callback_ != nullptr) {
      key_received_callback_(recovered, frame->src_node, key_received_context_);
    }
    crypto::secure_zero(recovered, sizeof(recovered));
    return;
  }
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
