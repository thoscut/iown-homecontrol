/**
 * @file iohome_2w.cpp
 * @brief io-homecontrol 2-Way Mode Features Implementation
 * @author iown-homecontrol project
 */

#include "iohome_2w.h"
#include "iohome_crypto.h"
#include <string.h>

namespace iohome {
namespace mode2w {

// ============================================================================
// Frequency Hopping Implementation
// ============================================================================

ChannelHopper::ChannelHopper()
  : current_channel_(ChannelState::CHANNEL_2),
    last_hop_time_us_(0),
    hop_interval_us_(2700),  // 2.7 ms
    enabled_(false)
{
}

void ChannelHopper::begin(float hop_interval_ms) {
  if (hop_interval_ms <= 0.0f) {
    hop_interval_ms = CHANNEL_HOP_TIME_MS;
  }
  hop_interval_us_ = static_cast<unsigned long>(hop_interval_ms * 1000.0f);
  if (hop_interval_us_ == 0) {
    hop_interval_us_ = 1;
  }
  last_hop_time_us_ = 0;
  current_channel_ = ChannelState::CHANNEL_2;  // Start on the primary channel
  enabled_ = false;
}

bool ChannelHopper::update_us(unsigned long current_time_us) {
  if (!enabled_) {
    return false;
  }

  // Do the arithmetic in 32 bits so the unsigned subtraction wraps at 2^32 -
  // the width of Arduino micros() - on every platform, including a 64-bit host.
  const uint32_t now = static_cast<uint32_t>(current_time_us);
  const uint32_t elapsed_us = now - last_hop_time_us_;

  if (elapsed_us >= hop_interval_us_) {
    next_channel();
    last_hop_time_us_ = now;
    return true;
  }

  return false;
}

bool ChannelHopper::update(unsigned long current_time_ms) {
  // Millisecond resolution cannot express the 2.7 ms dwell time; this wrapper
  // exists for tests and for slow-hop experiments only.
  return update_us(current_time_ms * 1000UL);
}

float ChannelHopper::frequency_of(ChannelState channel) {
  switch (channel) {
    case ChannelState::CHANNEL_1:
      return FREQUENCY_CHANNEL_1;
    case ChannelState::CHANNEL_3:
      return FREQUENCY_CHANNEL_3;
    case ChannelState::CHANNEL_2:
    default:
      return FREQUENCY_CHANNEL_2;
  }
}

float ChannelHopper::get_current_frequency() const {
  return frequency_of(current_channel_);
}

void ChannelHopper::reset(unsigned long current_time_us) {
  current_channel_ = ChannelState::CHANNEL_2;
  last_hop_time_us_ = static_cast<uint32_t>(current_time_us);
}

unsigned long ChannelHopper::time_until_next_hop_us(unsigned long current_time_us) const {
  // 32-bit math, as in update_us(): the counter wraps at 2^32 everywhere.
  const uint32_t elapsed_us = static_cast<uint32_t>(current_time_us) - last_hop_time_us_;

  if (elapsed_us >= hop_interval_us_) {
    return 0;
  }

  return hop_interval_us_ - elapsed_us;
}

void ChannelHopper::next_channel() {
  switch (current_channel_) {
    case ChannelState::CHANNEL_1:
      current_channel_ = ChannelState::CHANNEL_2;
      break;
    case ChannelState::CHANNEL_2:
      current_channel_ = ChannelState::CHANNEL_3;
      break;
    case ChannelState::CHANNEL_3:
    default:
      current_channel_ = ChannelState::CHANNEL_1;
      break;
  }
}

// ============================================================================
// Challenge-Response Authentication Implementation
// ============================================================================

namespace {

/// Command id + parameters, the input the 2W MAC is computed over.
size_t request_bytes(const frame::IoFrame* frame, uint8_t out[AuthenticationManager::MAX_REQUEST_SIZE]) {
  out[0] = frame->command_id;
  size_t len = frame->data_len;
  if (len > FRAME_MAX_DATA_SIZE) {
    len = FRAME_MAX_DATA_SIZE;
  }
  if (len > 0) {
    memcpy(&out[1], frame->data, len);
  }
  return 1 + len;
}

} // namespace

AuthenticationManager::AuthenticationManager()
  : key_set_(false),
    peer_known_(false),
    state_(ChallengeState::IDLE),
    state_timestamp_ms_(0),
    challenge_timeout_ms_(DEFAULT_CHALLENGE_TIMEOUT_MS),
    session_timeout_ms_(DEFAULT_SESSION_TIMEOUT_MS),
    held_valid_(false),
    verified_ready_(false),
    pending_len_(0),
    pending_ts_ms_(0),
    pending_answers_(0),
    pending_valid_(false),
    response_window_ms_(DEFAULT_RESPONSE_WINDOW_MS)
{
  memset(system_key_, 0, AES_KEY_SIZE);
  memset(current_challenge_, 0, HMAC_SIZE);
  memset(peer_node_, 0, NODE_ID_SIZE);
  memset(own_node_, 0, NODE_ID_SIZE);
  memset(&held_request_, 0, sizeof(held_request_));
  memset(pending_request_, 0, sizeof(pending_request_));
  memset(pending_dest_, 0, NODE_ID_SIZE);
  memset(pending_src_, 0, NODE_ID_SIZE);
}

AuthenticationManager::~AuthenticationManager() {
  crypto::secure_zero(system_key_, AES_KEY_SIZE);
  crypto::secure_zero(current_challenge_, HMAC_SIZE);
  crypto::secure_zero(&held_request_, sizeof(held_request_));
  crypto::secure_zero(pending_request_, sizeof(pending_request_));
}

bool AuthenticationManager::begin(const uint8_t system_key[AES_KEY_SIZE]) {
  if (system_key == nullptr) {
    return false;
  }
  memcpy(system_key_, system_key, AES_KEY_SIZE);
  key_set_ = true;
  reset();
  clear_pending_request();
  return true;
}

bool AuthenticationManager::generate_challenge(uint8_t challenge_out[HMAC_SIZE],
                                              unsigned long now_ms) {
  if (challenge_out == nullptr) {
    return false;
  }

  // A predictable challenge lets an attacker precompute a valid response, so
  // refuse to emit one rather than fall back to a PRNG.
  if (!crypto::random_bytes(challenge_out, HMAC_SIZE)) {
    reset();
    return false;
  }

  // A new challenge supersedes whatever the previous one was bound to.
  reset();
  memcpy(current_challenge_, challenge_out, HMAC_SIZE);
  state_ = ChallengeState::CHALLENGE_SENT;

  // Start the timeout now. Leaving the timestamp at zero made every handshake
  // attempted more than challenge_timeout_ms_ after boot expire immediately.
  state_timestamp_ms_ = now_ms;
  return true;
}

bool AuthenticationManager::create_challenge_request(
  frame::IoFrame* frame,
  const uint8_t dest_node[NODE_ID_SIZE],
  const uint8_t src_node[NODE_ID_SIZE],
  unsigned long now_ms
) {
  if (frame == nullptr || dest_node == nullptr || src_node == nullptr || !key_set_) {
    return false;
  }

  frame::init_frame(frame, false);  // 2W mode
  frame::set_destination(frame, dest_node);
  frame::set_source(frame, src_node);

  uint8_t challenge[HMAC_SIZE];
  if (!generate_challenge(challenge, now_ms)) {
    return false;
  }

  // generate_challenge() clears these; record them after it, not before.
  memcpy(peer_node_, dest_node, NODE_ID_SIZE);
  memcpy(own_node_, src_node, NODE_ID_SIZE);
  peer_known_ = true;

  const bool ok = frame::set_command(frame, CMD_CHALLENGE_REQUEST, challenge, HMAC_SIZE) &&
                  frame::finalize_frame_plain(frame);
  crypto::secure_zero(challenge, sizeof(challenge));
  if (!ok) {
    reset();
  }
  return ok;
}

bool AuthenticationManager::create_challenge_for(frame::IoFrame* challenge_out,
                                                 const frame::IoFrame* request,
                                                 const uint8_t own_node[NODE_ID_SIZE],
                                                 unsigned long now_ms) {
  if (challenge_out == nullptr || request == nullptr || own_node == nullptr || !key_set_) {
    return false;
  }

  // Do not let a fresh frame cancel a challenge still waiting for its answer:
  // anyone can send plain frames, so replacing would make every handshake
  // trivially disruptable.
  if (get_state(now_ms) == ChallengeState::CHALLENGE_SENT) {
    return false;
  }

  if (!create_challenge_request(challenge_out, request->src_node, own_node, now_ms)) {
    return false;
  }

  memcpy(&held_request_, request, sizeof(held_request_));
  held_valid_ = true;
  return true;
}

bool AuthenticationManager::verify_challenge_response(const frame::IoFrame* frame,
                                                      unsigned long now_ms) {
  if (frame == nullptr || !key_set_) {
    return false;
  }

  if (state_ != ChallengeState::CHALLENGE_SENT) {
    return false;  // No challenge outstanding
  }

  // Unsigned subtraction is wrap-safe, so this stays correct past the
  // ~49 day millis() rollover.
  if ((now_ms - state_timestamp_ms_) > challenge_timeout_ms_) {
    reset();
    return false;  // Timeout
  }

  if (frame->command_id != CMD_CHALLENGE_RESPONSE) {
    return false;
  }

  // Only an answer from the node we challenged, addressed to us, is ours to
  // judge. Anything else belongs to another conversation and must leave this
  // handshake alone - see the note on the declaration.
  if (!peer_known_ ||
      memcmp(frame->src_node, peer_node_, NODE_ID_SIZE) != 0 ||
      memcmp(frame->dest_node, own_node_, NODE_ID_SIZE) != 0) {
    return false;
  }

  // From here on the frame is our peer's answer, and any failure burns the
  // nonce: leaving it live would let an attacker grind guesses against it.
  //
  // The answer is the 6-byte payload, not a trailer. A frame that claims a
  // trailer, a wrong-sized payload, a bad CRC, or no held command to check it
  // against all fail closed - the old code accepted a 0x3D with no MAC at all,
  // because validate_frame() called a plain frame "nothing to verify".
  bool ok = !frame->authenticated && frame->data_len == HMAC_SIZE && held_valid_ &&
            frame::validate_frame(frame);  // CRC only

  if (ok) {
    uint8_t request[MAX_REQUEST_SIZE];
    const size_t request_len = request_bytes(&held_request_, request);
    ok = crypto::verify_hmac(request, request_len, frame->data, current_challenge_,
                             system_key_, /*is_2w=*/true);
    crypto::secure_zero(request, sizeof(request));
  }

  if (!ok) {
    reset();
    return false;
  }

  // Consume the nonce: a replayed 0x3D finds no CHALLENGE_SENT state. Keep the
  // peer so is_authenticated_with() can report who proved the key.
  crypto::secure_zero(current_challenge_, HMAC_SIZE);
  state_ = ChallengeState::AUTHENTICATED;
  state_timestamp_ms_ = now_ms;
  verified_ready_ = true;
  return true;
}

bool AuthenticationManager::take_verified_request(frame::IoFrame* out) {
  if (out == nullptr || !verified_ready_ || !held_valid_) {
    return false;
  }
  memcpy(out, &held_request_, sizeof(held_request_));
  verified_ready_ = false;
  held_valid_ = false;
  crypto::secure_zero(&held_request_, sizeof(held_request_));
  return true;
}

bool AuthenticationManager::note_request_sent(const frame::IoFrame* request,
                                              unsigned long now_ms) {
  if (request == nullptr) {
    return false;
  }
  pending_len_ = request_bytes(request, pending_request_);
  memcpy(pending_dest_, request->dest_node, NODE_ID_SIZE);
  memcpy(pending_src_, request->src_node, NODE_ID_SIZE);
  pending_ts_ms_ = now_ms;
  pending_answers_ = 0;
  pending_valid_ = true;
  return true;
}

void AuthenticationManager::clear_pending_request() {
  pending_valid_ = false;
  pending_len_ = 0;
  pending_answers_ = 0;
  crypto::secure_zero(pending_request_, sizeof(pending_request_));
  memset(pending_dest_, 0, NODE_ID_SIZE);
  memset(pending_src_, 0, NODE_ID_SIZE);
}

bool AuthenticationManager::has_pending_request(unsigned long now_ms) const {
  return pending_valid_ && (now_ms - pending_ts_ms_) <= response_window_ms_ &&
         pending_answers_ < MAX_ANSWERS_PER_REQUEST;
}

bool AuthenticationManager::answer_challenge(const frame::IoFrame* challenge_frame,
                                             frame::IoFrame* response_out,
                                             unsigned long now_ms) {
  if (challenge_frame == nullptr || response_out == nullptr || !key_set_) {
    return false;
  }

  if (challenge_frame->command_id != CMD_CHALLENGE_REQUEST ||
      challenge_frame->authenticated || challenge_frame->data_len != HMAC_SIZE) {
    return false;
  }

  if (!pending_valid_) {
    return false;  // We sent nothing: never sign a nonce for a request we did not make
  }
  if ((now_ms - pending_ts_ms_) > response_window_ms_) {
    clear_pending_request();
    return false;
  }
  if (pending_answers_ >= MAX_ANSWERS_PER_REQUEST) {
    return false;
  }

  // The challenge must come from the node our request went to, addressed to us.
  if (memcmp(challenge_frame->src_node, pending_dest_, NODE_ID_SIZE) != 0 ||
      memcmp(challenge_frame->dest_node, pending_src_, NODE_ID_SIZE) != 0) {
    return false;
  }

  if (!create_challenge_response(response_out, pending_dest_, pending_src_,
                                 challenge_frame->data, pending_request_, pending_len_)) {
    return false;
  }
  pending_answers_++;
  return true;
}

bool AuthenticationManager::create_challenge_response(
  frame::IoFrame* frame,
  const uint8_t dest_node[NODE_ID_SIZE],
  const uint8_t src_node[NODE_ID_SIZE],
  const uint8_t challenge[HMAC_SIZE],
  const uint8_t* request_data,
  size_t request_len
) {
  if (frame == nullptr || dest_node == nullptr || src_node == nullptr ||
      challenge == nullptr || request_data == nullptr || request_len == 0 ||
      request_len > MAX_REQUEST_SIZE || !key_set_) {
    return false;
  }

  uint8_t answer[HMAC_SIZE];
  if (!crypto::create_2w_hmac(request_data, request_len, challenge, system_key_, answer)) {
    return false;
  }

  frame::init_frame(frame, false);  // 2W mode
  frame::set_destination(frame, dest_node);
  frame::set_source(frame, src_node);

  const bool ok = frame::set_command(frame, CMD_CHALLENGE_RESPONSE, answer, HMAC_SIZE) &&
                  frame::finalize_frame_plain(frame);
  crypto::secure_zero(answer, sizeof(answer));
  return ok;
}

bool AuthenticationManager::recover_2w_key(const frame::IoFrame* key_frame,
                                           const uint8_t* request_frame_data,
                                           size_t request_data_len,
                                           uint8_t key_out[AES_KEY_SIZE]) const {
  if (key_frame == nullptr || key_out == nullptr) {
    return false;
  }
  if (key_frame->command_id != CMD_KEY_TRANSFER || key_frame->data_len < AES_KEY_SIZE) {
    return false;
  }
  // Never unmask against a challenge we never generated. current_challenge_ is
  // all zeros until this node sends a 0x3C; unmasking with it would turn a
  // fully attacker-known mask into an attacker-chosen key. An all-zero nonce is
  // not a value the CSPRNG produces in practice, so rejecting it costs nothing.
  bool challenge_set = false;
  for (size_t i = 0; i < HMAC_SIZE; i++) {
    challenge_set |= (current_challenge_[i] != 0);
  }
  if (!challenge_set) {
    return false;
  }
  // Uses current_challenge_ - the nonce this node put in the 0x3C it sent - plus
  // the request frame and the public transfer key. No system key involved.
  return crypto::decrypt_2w_key(key_frame->data, request_frame_data, request_data_len,
                                current_challenge_, key_out);
}

bool AuthenticationManager::has_active_challenge(unsigned long now_ms) {
  return get_state(now_ms) == ChallengeState::CHALLENGE_SENT;
}

bool AuthenticationManager::is_authenticated_with(const uint8_t peer[NODE_ID_SIZE],
                                                  unsigned long now_ms) {
  return peer != nullptr && is_authenticated(now_ms) && peer_known_ &&
         memcmp(peer_node_, peer, NODE_ID_SIZE) == 0;
}

ChallengeState AuthenticationManager::get_state(unsigned long now_ms) {
  // Wrap-safe: unsigned subtraction stays correct past the millis() rollover.
  const unsigned long elapsed = now_ms - state_timestamp_ms_;

  const bool challenge_expired =
    state_ == ChallengeState::CHALLENGE_SENT && elapsed > challenge_timeout_ms_;
  const bool session_expired =
    state_ == ChallengeState::AUTHENTICATED && elapsed > session_timeout_ms_;

  if (challenge_expired || session_expired) {
    reset();
  }

  return state_;
}

void AuthenticationManager::reset() {
  peer_known_ = false;
  memset(peer_node_, 0, NODE_ID_SIZE);
  memset(own_node_, 0, NODE_ID_SIZE);
  state_ = ChallengeState::IDLE;
  crypto::secure_zero(current_challenge_, HMAC_SIZE);
  state_timestamp_ms_ = 0;
  held_valid_ = false;
  verified_ready_ = false;
  crypto::secure_zero(&held_request_, sizeof(held_request_));
}

// ============================================================================
// Beacon Handling Implementation
// ============================================================================

BeaconHandler::BeaconHandler()
  : beacon_received_(false),
    beacon_count_(0)
{
  memset(&last_beacon_, 0, sizeof(BeaconInfo));
}

void BeaconHandler::begin() {
  beacon_received_ = false;
  beacon_count_ = 0;
  memset(&last_beacon_, 0, sizeof(BeaconInfo));
}

bool BeaconHandler::process_beacon(const frame::IoFrame* frame, int16_t rssi, float snr,
                                   unsigned long now_ms) {
  if (frame == nullptr) {
    return false;
  }

  if (!(frame->ctrl_byte_1 & CTRL1_USE_BEACON)) {
    return false;  // Not a beacon frame
  }

  memcpy(last_beacon_.node_id, frame->src_node, NODE_ID_SIZE);

  const uint8_t copy_len =
    frame->data_len < FRAME_MAX_DATA_SIZE ? frame->data_len : FRAME_MAX_DATA_SIZE;

  if (copy_len > 0) {
    last_beacon_.type = static_cast<BeaconType>(frame->data[0]);
    memcpy(last_beacon_.data, frame->data, copy_len);
  } else {
    last_beacon_.type = BeaconType::SYNC_BEACON;
  }

  // Record the length actually stored, not the claimed one.
  last_beacon_.data_len = copy_len;
  last_beacon_.rssi = rssi;
  last_beacon_.snr = snr;
  last_beacon_.timestamp_ms = now_ms;
  beacon_received_ = true;
  beacon_count_++;

  return true;
}

bool BeaconHandler::get_last_beacon(BeaconInfo* info) const {
  if (info == nullptr || !beacon_received_) {
    return false;
  }

  memcpy(info, &last_beacon_, sizeof(BeaconInfo));
  return true;
}

bool BeaconHandler::has_recent_beacon(unsigned long now_ms, unsigned long timeout_ms) const {
  if (!beacon_received_) {
    return false;
  }

  return (now_ms - last_beacon_.timestamp_ms) <= timeout_ms;
}

unsigned long BeaconHandler::time_since_last_beacon(unsigned long now_ms) const {
  if (!beacon_received_) {
    return 0xFFFFFFFFUL;  // Max value if no beacon was received
  }

  return now_ms - last_beacon_.timestamp_ms;
}

// ============================================================================
// Discovery and Pairing Implementation
// ============================================================================

DiscoveryManager::DiscoveryManager()
  : state_(DiscoveryState::IDLE),
    discovery_start_time_(0),
    discovery_timeout_(0),
    discovery_device_type_(0xFF),
    discovered_count_(0)
{
  memset(own_node_id_, 0, NODE_ID_SIZE);
  memset(discovered_devices_, 0, sizeof(discovered_devices_));
}

bool DiscoveryManager::begin(const uint8_t own_node_id[NODE_ID_SIZE]) {
  if (own_node_id == nullptr) {
    return false;
  }
  memcpy(own_node_id_, own_node_id, NODE_ID_SIZE);
  state_ = DiscoveryState::IDLE;
  discovered_count_ = 0;
  return true;
}

void DiscoveryManager::start_discovery(uint8_t device_type, unsigned long timeout_ms,
                                       unsigned long now_ms) {
  state_ = DiscoveryState::DISCOVERING;
  discovery_start_time_ = now_ms;
  discovery_timeout_ = timeout_ms;
  discovery_device_type_ = device_type;
  discovered_count_ = 0;
}

void DiscoveryManager::stop_discovery() {
  state_ = (discovered_count_ > 0) ? DiscoveryState::FOUND : DiscoveryState::IDLE;
}

bool DiscoveryManager::update(unsigned long now_ms) {
  if (state_ != DiscoveryState::DISCOVERING) {
    return false;
  }

  if (discovery_timeout_ > 0 && (now_ms - discovery_start_time_) >= discovery_timeout_) {
    state_ = (discovered_count_ > 0) ? DiscoveryState::FOUND : DiscoveryState::TIMED_OUT;
    return false;
  }

  return true;
}

bool DiscoveryManager::create_discovery_request(frame::IoFrame* frame, uint8_t device_type) {
  if (frame == nullptr) {
    return false;
  }

  // Discovery is a 2W broadcast, see the capture in docs/linklayer.md:
  //   C8 00 00003B F00F00 28 1234
  frame::init_frame(frame, false);
  frame::set_destination(frame, ADDRESS_BROADCAST);
  frame::set_source(frame, own_node_id_);

  // Command 0x28 carries no parameters; the device type is only a local
  // filter applied to the answers.
  if (!frame::set_command(frame, CMD_DISCOVER, nullptr, 0)) {
    return false;
  }

  discovery_device_type_ = device_type;

  // Discovery is unauthenticated - the peers have no shared key yet.
  return frame::finalize_frame_plain(frame);
}

bool DiscoveryManager::process_discovery_response(const frame::IoFrame* frame, int16_t rssi,
                                                  unsigned long now_ms) {
  if (frame == nullptr) {
    return false;
  }

  if (!update(now_ms)) {
    return false;  // Not discovering, or the window has closed
  }

  // Only discovery answers populate the device list; ignoring everything else
  // keeps ordinary traffic from being mistaken for a discovered device.
  if (frame->command_id != CMD_DISCOVER_ANSWER &&
      frame->command_id != CMD_DISCOVER_REMOTE_ANSWER) {
    return false;
  }

  if (discovered_count_ >= MAX_DISCOVERED_DEVICES) {
    return false;
  }

  for (size_t i = 0; i < discovered_count_; i++) {
    if (memcmp(discovered_devices_[i].node_id, frame->src_node, NODE_ID_SIZE) == 0) {
      return false;  // Already in the list
    }
  }

  DiscoveredDevice* device = &discovered_devices_[discovered_count_];
  memcpy(device->node_id, frame->src_node, NODE_ID_SIZE);

  // Discover Answer payload, from docs/commands.md:
  //
  //   [0..1] node type and sub-type, 16 bits, most significant byte first
  //   [2..4] node address
  //   [5]    manufacturer (OEM) ID
  //   [6]    multi info byte
  //   [7..8] timestamp
  //
  // Every field is optional here only in the sense that a short answer should
  // not be read past its end; a well-formed one carries all of them.
  device->node_type = 0;
  device->type = 0;
  device->subtype = 0;
  device->manufacturer = 0;
  device->multi_info = 0;
  device->device_timestamp = 0;

  if (frame->data_len >= 2) {
    device->node_type = static_cast<uint16_t>((static_cast<uint16_t>(frame->data[0]) << 8) |
                                              frame->data[1]);
    device->type = node_type_of(device->node_type);
    device->subtype = node_subtype_of(device->node_type);
  }

  // The node address at [2..4] repeats the frame's source, which is already in
  // node_id, so it is read only to get past it.
  if (frame->data_len >= 6) {
    device->manufacturer = frame->data[5];
  }
  if (frame->data_len >= 7) {
    device->multi_info = frame->data[6];
  }
  if (frame->data_len >= 9) {
    device->device_timestamp = static_cast<uint16_t>((static_cast<uint16_t>(frame->data[7]) << 8) |
                                                     frame->data[8]);
  }

  device->rssi = rssi;
  device->timestamp_ms = now_ms;

  discovered_count_++;
  // Stay in DISCOVERING so the remaining devices are collected too.
  return true;
}

bool DiscoveryManager::get_discovered_device(size_t index, DiscoveredDevice* device) const {
  if (device == nullptr || index >= discovered_count_) {
    return false;
  }

  memcpy(device, &discovered_devices_[index], sizeof(DiscoveredDevice));
  return true;
}

bool DiscoveryManager::create_key_transfer_1w(
  frame::IoFrame* frame,
  const uint8_t dest_node[NODE_ID_SIZE],
  const uint8_t src_node[NODE_ID_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  uint8_t manufacturer,
  uint16_t sequence
) {
  if (frame == nullptr || dest_node == nullptr || src_node == nullptr || system_key == nullptr) {
    return false;
  }

  frame::init_frame(frame, true);
  frame::set_destination(frame, dest_node);
  frame::set_source(frame, src_node);

  // Payload per docs/commands.md "30: Send 1W Key":
  //   encrypted key (16) | manufacturer (1) | reserved (1) | sequence (2)
  //
  // The key is masked with the *sender's* own address - the node that owns the
  // key being transferred - not the destination. docs/linklayer.md: "an initial
  // value that consists in its address repeated". The receiver unmasks with the
  // frame's source, so masking with the destination hands it noise. This
  // reproduces the documented vector (node ABCDEF -> 7E60491F...01) and matches
  // both real captures and rspaargaren/iohomecontrol; the earlier dest_node was
  // self-consistent in the round-trip test but would never interoperate.
  uint8_t params[AES_KEY_SIZE + 4];
  if (!crypto::encrypt_1w_key(system_key, src_node, params)) {
    return false;
  }

  params[AES_KEY_SIZE + 0] = manufacturer;
  params[AES_KEY_SIZE + 1] = 0x01;
  params[AES_KEY_SIZE + 2] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
  params[AES_KEY_SIZE + 3] = static_cast<uint8_t>(sequence & 0xFF);

  const bool set_ok = frame::set_command(frame, CMD_SEND_1W_KEY, params, sizeof(params));
  crypto::secure_zero(params, sizeof(params));

  if (!set_ok) {
    return false;
  }

  // Key transfer carries no MAC: the receiver does not have the key yet.
  return frame::finalize_frame_plain(frame);
}

bool DiscoveryManager::create_key_transfer_2w(
  frame::IoFrame* frame,
  const uint8_t dest_node[NODE_ID_SIZE],
  const uint8_t src_node[NODE_ID_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  const uint8_t challenge[HMAC_SIZE],
  const uint8_t* request_frame_data,
  size_t request_data_len
) {
  if (frame == nullptr || dest_node == nullptr || src_node == nullptr ||
      system_key == nullptr || challenge == nullptr) {
    return false;
  }
  if (request_frame_data == nullptr || request_data_len == 0) {
    return false;
  }

  frame::init_frame(frame, false);
  frame::set_destination(frame, dest_node);
  frame::set_source(frame, src_node);

  uint8_t encrypted_key[AES_KEY_SIZE];
  if (!crypto::encrypt_2w_key(system_key, request_frame_data, request_data_len,
                              challenge, encrypted_key)) {
    return false;
  }

  const bool set_ok = frame::set_command(frame, CMD_KEY_TRANSFER, encrypted_key, AES_KEY_SIZE);
  crypto::secure_zero(encrypted_key, sizeof(encrypted_key));

  if (!set_ok) {
    return false;
  }

  return frame::finalize_frame_plain(frame);
}

bool DiscoveryManager::create_remove_1w_controller(
  frame::IoFrame* frame,
  const uint8_t dest_node[NODE_ID_SIZE],
  const uint8_t src_node[NODE_ID_SIZE]
) {
  if (frame == nullptr || dest_node == nullptr || src_node == nullptr) {
    return false;
  }

  frame::init_frame(frame, true);
  frame::set_destination(frame, dest_node);
  frame::set_source(frame, src_node);

  if (!frame::set_command(frame, CMD_REMOVE_1W_CONTROLLER, nullptr, 0)) {
    return false;
  }

  return frame::finalize_frame_plain(frame);
}

} // namespace mode2w
} // namespace iohome
