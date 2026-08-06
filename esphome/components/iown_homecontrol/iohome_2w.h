/**
 * @file iohome_2w.h
 * @brief io-homecontrol 2-Way Mode Features
 * @author iown-homecontrol project
 *
 * Advanced features for 2-Way mode including:
 * - Frequency hopping (FHSS)
 * - Challenge-response authentication
 * - Beacon handling
 * - Discovery and pairing
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "iohome_constants.h"
#include "iohome_frame.h"

namespace iohome {
namespace mode2w {

// ============================================================================
// Frequency Hopping (FHSS)
// ============================================================================

/**
 * @brief Channel hopping state
 */
enum class ChannelState : uint8_t {
  CHANNEL_1 = 0,  // 868.25 MHz
  CHANNEL_2 = 1,  // 868.95 MHz (primary)
  CHANNEL_3 = 2   // 869.85 MHz
};

/**
 * @brief Frequency Hopping State Machine
 *
 * Manages channel switching for 2W mode with a 2.7 ms dwell time.
 *
 * The dwell time is shorter than a millisecond tick, so all timing is kept in
 * microseconds. Feed update_us() from micros(); update() is a millisecond
 * convenience wrapper whose resolution is too coarse for the real protocol
 * timing and is only appropriate for tests and slow-hop experiments.
 */
class ChannelHopper {
public:
  ChannelHopper();

  /**
   * @brief Initialize channel hopper
   *
   * @param hop_interval_ms Channel hop interval in milliseconds (default: 2.7)
   */
  void begin(float hop_interval_ms = CHANNEL_HOP_TIME_MS);

  /**
   * @brief Update the state machine using a microsecond timestamp
   *
   * @param current_time_us Current time in microseconds (e.g. micros())
   * @return true if the channel changed
   */
  bool update_us(unsigned long current_time_us);

  /**
   * @brief Update the state machine using a millisecond timestamp
   *
   * @param current_time_ms Current time in milliseconds (e.g. millis())
   * @return true if the channel changed
   */
  bool update(unsigned long current_time_ms);

  /**
   * @brief Get current channel state
   */
  ChannelState get_current_channel() const { return current_channel_; }

  /**
   * @brief Get current frequency in MHz
   */
  float get_current_frequency() const;

  /**
   * @brief Get the frequency of a specific channel in MHz
   */
  static float frequency_of(ChannelState channel);

  /**
   * @brief Reset to the primary channel
   *
   * @param current_time_us Current time in microseconds
   */
  void reset(unsigned long current_time_us = 0);

  /**
   * @brief Enable/disable hopping
   */
  void set_enabled(bool enabled) { enabled_ = enabled; }

  /**
   * @brief Check if hopping is enabled
   */
  bool is_enabled() const { return enabled_; }

  /**
   * @brief Get time until the next hop in microseconds
   *
   * @param current_time_us Current time in microseconds
   */
  unsigned long time_until_next_hop_us(unsigned long current_time_us) const;

  /**
   * @brief Configured dwell time per channel in microseconds
   */
  unsigned long get_hop_interval_us() const { return hop_interval_us_; }

protected:
  ChannelState current_channel_;
  unsigned long last_hop_time_us_;
  unsigned long hop_interval_us_;
  bool enabled_;

  void next_channel();
};

// ============================================================================
// Challenge-Response Authentication
// ============================================================================

/**
 * @brief Challenge-Response state
 */
enum class ChallengeState : uint8_t {
  IDLE,             // No authentication in progress
  CHALLENGE_SENT,   // Waiting for challenge response
  AUTHENTICATED     // Successfully authenticated
};

/**
 * @brief Challenge-Response Authentication Manager
 *
 * Handles 2W authentication using a challenge-response mechanism.
 *
 * Challenges are drawn from the platform CSPRNG. If the platform has no secure
 * random source, generate_challenge() fails rather than emitting a predictable
 * nonce - a predictable challenge would let an attacker precompute a valid
 * response and defeat the whole handshake.
 */
class AuthenticationManager {
public:
  /// A session stays authenticated for this long before a new handshake is required.
  static constexpr uint32_t DEFAULT_SESSION_TIMEOUT_MS = 30000;
  /// A pending challenge expires after this long without an answer.
  static constexpr uint32_t DEFAULT_CHALLENGE_TIMEOUT_MS = 5000;

  AuthenticationManager();

  /**
   * @brief Initialize the authentication manager
   *
   * @param system_key System key for MAC calculation (16 bytes)
   * @return false if `system_key` is null
   */
  bool begin(const uint8_t system_key[AES_KEY_SIZE]);

  /**
   * @brief Generate a new random challenge
   *
   * Starts the challenge timeout, so the caller must pass the current time -
   * without it the challenge is timestamped at zero and every handshake
   * attempted more than challenge_timeout_ms_ after boot expires instantly.
   *
   * @param challenge_out Output buffer (6 bytes)
   * @param now_ms Current time in milliseconds
   * @return false when no secure random source is available, or on bad input
   */
  bool generate_challenge(uint8_t challenge_out[HMAC_SIZE], unsigned long now_ms);

  /**
   * @brief Create a challenge request frame (command 0x3C)
   *
   * @param now_ms Current time in milliseconds; starts the challenge timeout
   * @return true on success
   */
  bool create_challenge_request(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE],
    unsigned long now_ms
  );

  /**
   * @brief Create a challenge response frame (command 0x3D)
   *
   * @param received_challenge Challenge from the request (6 bytes)
   * @return true on success
   */
  bool create_challenge_response(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE],
    const uint8_t received_challenge[HMAC_SIZE]
  );

  /**
   * @brief Verify a challenge response
   *
   * On success the challenge is consumed: a replayed response is rejected
   * because the manager leaves the CHALLENGE_SENT state. A response from the
   * node we challenged that fails its MAC consumes the challenge too, so an
   * attacker cannot grind guesses against one known nonce.
   *
   * A frame from any *other* node, or addressed to any other node, is ignored
   * without touching the state. That distinction matters: burning the nonce on
   * every failed 0x3D meant a neighbouring pair of devices doing their own
   * exchange - or six arbitrary bytes from anyone with a radio - cancelled a
   * handshake in progress, and the genuine answer then arrived to an IDLE
   * state. The peer is remembered by create_challenge_request(); a challenge
   * made with generate_challenge() alone has no peer to compare against and is
   * verified on the MAC only.
   *
   * @param frame Received response frame
   * @param now_ms Current time in milliseconds
   * @return true if valid
   */
  bool verify_challenge_response(const frame::IoFrame* frame, unsigned long now_ms);

  /**
   * @brief Get the current challenge (6 bytes)
   *
   * Only meaningful while has_active_challenge() is true; the buffer is zeroed
   * otherwise.
   */
  const uint8_t* get_current_challenge() const { return current_challenge_; }

  /**
   * @brief Whether a challenge is available to bind MACs to
   *
   * True from the moment a challenge is generated until the session expires or
   * is reset. Expires the state as a side effect, like get_state().
   *
   * @param now_ms Current time in milliseconds
   */
  bool has_active_challenge(unsigned long now_ms);

  /**
   * @brief Get the authentication state, expiring it if it has timed out
   *
   * @param now_ms Current time in milliseconds
   */
  ChallengeState get_state(unsigned long now_ms);

  /**
   * @brief Get the last known authentication state without expiring it
   */
  ChallengeState peek_state() const { return state_; }

  /**
   * @brief Whether the session is currently authenticated
   */
  bool is_authenticated(unsigned long now_ms) { return get_state(now_ms) == ChallengeState::AUTHENTICATED; }

  /**
   * @brief Reset the authentication state and wipe the stored challenge
   */
  void reset();

  void set_challenge_timeout_ms(uint32_t timeout_ms) { challenge_timeout_ms_ = timeout_ms; }
  void set_session_timeout_ms(uint32_t timeout_ms) { session_timeout_ms_ = timeout_ms; }

protected:
  uint8_t system_key_[AES_KEY_SIZE];
  uint8_t current_challenge_[HMAC_SIZE];
  /// Who we challenged, and as whom - only a response between these two counts.
  uint8_t peer_node_[NODE_ID_SIZE];
  uint8_t own_node_[NODE_ID_SIZE];
  bool peer_known_;
  ChallengeState state_;
  unsigned long state_timestamp_ms_;
  uint32_t challenge_timeout_ms_;
  uint32_t session_timeout_ms_;
  bool key_set_;
};

// ============================================================================
// Beacon Handling
// ============================================================================

/**
 * @brief Beacon type
 */
enum class BeaconType : uint8_t {
  SYNC_BEACON = 0x00,       // Synchronization beacon
  DISCOVERY_BEACON = 0x01,  // Discovery beacon
  SYSTEM_BEACON = 0x02      // System announcement
};

/**
 * @brief Beacon information
 */
struct BeaconInfo {
  uint8_t node_id[NODE_ID_SIZE];
  BeaconType type;
  uint8_t data[FRAME_MAX_DATA_SIZE];
  uint8_t data_len;
  int16_t rssi;
  float snr;
  unsigned long timestamp_ms;
};

/**
 * @brief Beacon Handler
 *
 * Manages beacon reception and processing for 2W synchronization.
 */
class BeaconHandler {
public:
  BeaconHandler();

  void begin();

  /**
   * @brief Process a received beacon frame
   *
   * @param frame Received frame
   * @param rssi RSSI value
   * @param snr SNR value
   * @param now_ms Current time in milliseconds
   * @return true if the frame was a beacon and was recorded
   */
  bool process_beacon(const frame::IoFrame* frame, int16_t rssi, float snr, unsigned long now_ms);

  /**
   * @brief Copy out the last received beacon info
   *
   * @return true if beacon info is available
   */
  bool get_last_beacon(BeaconInfo* info) const;

  /**
   * @brief Check whether a beacon was received within `timeout_ms`
   */
  bool has_recent_beacon(unsigned long now_ms, unsigned long timeout_ms = 5000) const;

  /**
   * @brief Time since the last beacon, or 0xFFFFFFFF if none was received
   */
  unsigned long time_since_last_beacon(unsigned long now_ms) const;

  /// Total number of beacons processed.
  uint32_t beacon_count() const { return beacon_count_; }

protected:
  BeaconInfo last_beacon_;
  bool beacon_received_;
  uint32_t beacon_count_;
};

// ============================================================================
// Discovery and Pairing
// ============================================================================

/**
 * @brief Device discovery state
 */
enum class DiscoveryState : uint8_t {
  IDLE,           // Not discovering
  DISCOVERING,    // Discovery in progress
  FOUND,          // Discovery finished with at least one device
  TIMED_OUT       // Discovery window elapsed without any device
};

/**
 * @brief Discovered device information
 *
 * Field for field, this is what a Discover Answer (0x29) carries. It used to
 * be read as `type = data[0]`, `manufacturer = data[1]`, `version = data[2]`,
 * which is three fields at three wrong offsets: the type is 16 bits, so
 * data[1] is its low half, and data[2] is the first byte of the node address.
 * There is no protocol-version field in the answer at all.
 */
struct DiscoveredDevice {
  uint8_t node_id[NODE_ID_SIZE];

  /// Full 16-bit type field, comparable against NodeType.
  uint16_t node_type;
  /// The 10-bit type half, for grouping by kind of device.
  uint16_t type;
  /// The 6-bit sub-type half, which distinguishes variants of one type.
  uint8_t subtype;

  /// OEM ID; see the Manufacturer enumeration.
  uint8_t manufacturer;
  /// "Multi info byte" - undecoded, carried through as received.
  uint8_t multi_info;
  /// Device's own timestamp, as reported.
  uint16_t device_timestamp;

  int16_t rssi;
  unsigned long timestamp_ms;
};

/**
 * @brief Discovery Manager
 *
 * Handles device discovery and pairing workflows.
 */
class DiscoveryManager {
public:
  static constexpr size_t MAX_DISCOVERED_DEVICES = 32;

  DiscoveryManager();

  /**
   * @brief Initialize the discovery manager
   *
   * @param own_node_id This controller's node ID (3 bytes)
   * @return false if `own_node_id` is null
   */
  bool begin(const uint8_t own_node_id[NODE_ID_SIZE]);

  /**
   * @brief Start device discovery
   *
   * @param device_type Type of device to discover (0xFF for all)
   * @param timeout_ms Discovery timeout in milliseconds
   * @param now_ms Current time in milliseconds
   */
  void start_discovery(uint8_t device_type, unsigned long timeout_ms, unsigned long now_ms);

  /**
   * @brief Stop device discovery
   */
  void stop_discovery();

  /**
   * @brief Expire the discovery window if `timeout_ms` has elapsed
   *
   * @param now_ms Current time in milliseconds
   * @return true if discovery is still running
   */
  bool update(unsigned long now_ms);

  /**
   * @brief Create a discovery request frame (command 0x28)
   *
   * The frame is finalized as a plain frame - discovery is unauthenticated
   * because no key has been exchanged yet.
   *
   * @return true on success
   */
  bool create_discovery_request(frame::IoFrame* frame, uint8_t device_type);

  /**
   * @brief Process a discovery answer (command 0x29 / 0x2B)
   *
   * Frames carrying any other command ID are ignored, so ordinary traffic
   * does not end up in the discovered-device list.
   *
   * @return true if the frame was a new discovery answer
   */
  bool process_discovery_response(const frame::IoFrame* frame, int16_t rssi, unsigned long now_ms);

  size_t get_discovered_count() const { return discovered_count_; }

  /**
   * @brief Copy out a discovered device by index
   */
  bool get_discovered_device(size_t index, DiscoveredDevice* device) const;

  DiscoveryState get_state() const { return state_; }

  /**
   * @brief Create a 1W key transfer frame (command 0x30)
   *
   * Payload: encrypted key (16) | manufacturer (1) | reserved (1) | sequence (2)
   * The frame is finalized as a plain frame.
   *
   * @return true on success
   */
  bool create_key_transfer_1w(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE],
    const uint8_t system_key[AES_KEY_SIZE],
    uint8_t manufacturer = 0x00,
    uint16_t sequence = 0
  );

  /**
   * @brief Create a 2W key transfer frame (command 0x32)
   *
   * The key is masked with an IV derived from the frame that requested the
   * transfer, not from the challenge alone - see crypto::encrypt_2w_key(). The
   * peer builds the same IV from the frame it sent, so passing the wrong one
   * hands it a key that decrypts to noise.
   *
   * For the push direction documented in docs/linklayer.md the requesting frame
   * is the device's command 0x31 (ask challenge), i.e. a single byte with no
   * parameters. For a pull it is the controller's 0x38 (launch key transfer)
   * together with its challenge.
   *
   * @param request_frame_data Requesting frame: command ID plus parameters
   * @param request_data_len Length of @p request_frame_data
   * @return true on success
   */
  bool create_key_transfer_2w(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE],
    const uint8_t system_key[AES_KEY_SIZE],
    const uint8_t challenge[HMAC_SIZE],
    const uint8_t* request_frame_data,
    size_t request_data_len
  );

  /**
   * @brief Create a "remove 1W controller" frame (command 0x39)
   *
   * Sent before a 1W key transfer so the actuator drops the previous key.
   */
  bool create_remove_1w_controller(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE]
  );

protected:
  uint8_t own_node_id_[NODE_ID_SIZE];
  DiscoveryState state_;
  unsigned long discovery_start_time_;
  unsigned long discovery_timeout_;
  uint8_t discovery_device_type_;

  DiscoveredDevice discovered_devices_[MAX_DISCOVERED_DEVICES];
  size_t discovered_count_;
};

} // namespace mode2w
} // namespace iohome
