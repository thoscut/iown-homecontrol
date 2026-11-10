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

/// Discovery filter value that accepts every device type. See
/// DiscoveryManager::start_discovery().
constexpr uint16_t DISCOVER_ALL_TYPES = 0xFF;

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
  // Stored as a fixed 32 bits so the wrap happens at 2^32 on every platform,
  // matching Arduino micros() (a 32-bit unsigned long). On a 64-bit host,
  // `unsigned long` is 64 bits and would only wrap at 2^64, so the timestamp
  // math is done in uint32_t regardless of the caller's type.
  uint32_t last_hop_time_us_;
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
 * Implements 2W authentication as docs/linklayer.md ("2W Authentication")
 * documents it and the captures there show. 2W frames carry no MAC. Instead:
 *
 *   A -> B   command, plain                    48 04 FEEFEE F00F00 36 9A02
 *   B -> A   0x3C, plain, 6-byte challenge     0E 00 F00F00 FEEFEE 3C 123456789ABC 19DB
 *   A -> B   0x3D, plain, 6-byte answer        0E 04 FEEFEE F00F00 3D C7FDC0668818 B1E3
 *   B        executes the command
 *
 * The answer is create_2w_hmac(command id + parameters of A's original
 * command, B's challenge, system key). With the key 0102...1516 and the frames
 * above that is C7FDC0668818 - the documented capture.
 *
 * This class plays both parts, with separate state for each:
 *
 *  - **Initiator** (we sent the command): note_request_sent() remembers the
 *    command; answer_challenge() turns the peer's 0x3C into our 0x3D. It only
 *    answers for a request WE sent, to the node we sent it to, within
 *    response_window_ms. Signing whatever nonce anyone sends would be a
 *    chosen-input MAC oracle; signing only our own pending request is not
 *    useful to an attacker, because the actuator picks its own fresh nonce.
 *
 *  - **Challenger** (a peer sent us a command): create_challenge_for() holds
 *    the peer's command and emits a 0x3C with a CSPRNG nonce;
 *    verify_challenge_response() checks the peer's 0x3D in constant time
 *    against the MAC over the held command, and take_verified_request() then
 *    hands that command out, once.
 *
 * Challenges are drawn from the platform CSPRNG. If the platform has no secure
 * random source, generating one fails rather than emitting a predictable nonce.
 */
class AuthenticationManager {
public:
  /// After a successful verification, is_authenticated() stays true this long.
  static constexpr uint32_t DEFAULT_SESSION_TIMEOUT_MS = 30000;
  /// A pending challenge expires after this long without an answer.
  static constexpr uint32_t DEFAULT_CHALLENGE_TIMEOUT_MS = 5000;
  /// How long after sending a command we answer the peer's 0x3C for it.
  static constexpr uint32_t DEFAULT_RESPONSE_WINDOW_MS = 1000;
  /// How many 0x3C for one pending request we answer (the peer re-challenges
  /// when our 0x3D is lost).
  static constexpr uint8_t MAX_ANSWERS_PER_REQUEST = 3;
  /// Largest command (id + parameters) the MAC can cover.
  static constexpr size_t MAX_REQUEST_SIZE = 1 + FRAME_MAX_DATA_SIZE;

  AuthenticationManager();

  /// Wipes the system key and current challenge on destruction, matching the
  /// zeroization discipline the rest of the codebase applies to key material.
  ~AuthenticationManager();

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
   * Starts the challenge timeout, so the caller must pass the current time.
   *
   * @param challenge_out Output buffer (6 bytes)
   * @param now_ms Current time in milliseconds
   * @return false when no secure random source is available, or on bad input
   */
  bool generate_challenge(uint8_t challenge_out[HMAC_SIZE], unsigned long now_ms);

  // --------------------------------------------------------------------------
  // Challenger role
  // --------------------------------------------------------------------------

  /**
   * @brief Create a plain challenge frame (0x3C) not bound to any command
   *
   * Used by 2W pairing, where the 0x3C answers an "ask challenge" (0x31) and
   * its nonce masks the key transfer that follows (recover_2w_key()). A 0x3D
   * cannot be verified against it: there is no command to authenticate.
   *
   * @return true on success
   */
  bool create_challenge_request(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE],
    unsigned long now_ms
  );

  /**
   * @brief Challenge a command a peer sent us (emit 0x3C, hold the command)
   *
   * The 0x3C goes to the command's source, from its destination (us). The
   * command is held until verify_challenge_response() accepts the matching
   * 0x3D, or the challenge times out. A live challenge is not replaced, so a
   * stream of plain frames cannot cancel a handshake in progress.
   *
   * @param challenge_out Receives the 0x3C frame to transmit
   * @param request The plain command received from the peer
   * @param own_node Our node ID (the 0x3C's source)
   * @return false if a challenge is already pending, on bad input, or with no
   *         secure random source
   */
  bool create_challenge_for(frame::IoFrame* challenge_out,
                            const frame::IoFrame* request,
                            const uint8_t own_node[NODE_ID_SIZE],
                            unsigned long now_ms);

  /**
   * @brief Verify a challenge answer (0x3D) against the held command
   *
   * The 0x3D must be plain, come from the node we challenged, be addressed to
   * us, and carry exactly create_2w_hmac(held command, our nonce, key),
   * compared in constant time. A frame from any other node, or to any other
   * node, is ignored without touching the state - a neighbouring pair doing
   * their own exchange must not cancel ours. A 0x3D from our peer that fails
   * (wrong length or wrong MAC) consumes the challenge, so an attacker cannot
   * grind guesses against one nonce. On success the nonce is consumed too,
   * which makes a replayed 0x3D useless.
   *
   * @return true if the answer proves the key for the held command
   */
  bool verify_challenge_response(const frame::IoFrame* frame, unsigned long now_ms);

  /**
   * @brief Hand out the command a successful verification authenticated
   *
   * Returns it once; later calls return false until the next verification.
   */
  bool take_verified_request(frame::IoFrame* out);

  // --------------------------------------------------------------------------
  // Initiator role
  // --------------------------------------------------------------------------

  /**
   * @brief Remember a 2W command we are about to send, so its 0x3C can be answered
   *
   * Replaces any previous pending request.
   */
  bool note_request_sent(const frame::IoFrame* request, unsigned long now_ms);

  /// Forget the pending request (e.g. its transmission failed).
  void clear_pending_request();

  /// Whether a request of ours is still waiting for its challenge.
  bool has_pending_request(unsigned long now_ms) const;

  /**
   * @brief Answer a peer's 0x3C for our pending request (emit 0x3D)
   *
   * Only answers a plain 0x3C with a 6-byte challenge, from the node our
   * pending request went to, addressed to the node it came from, within
   * response_window_ms of sending, and at most MAX_ANSWERS_PER_REQUEST times.
   *
   * @param challenge_frame The 0x3C received
   * @param response_out Receives the 0x3D to transmit
   * @return true if response_out holds an answer to send
   */
  bool answer_challenge(const frame::IoFrame* challenge_frame,
                        frame::IoFrame* response_out,
                        unsigned long now_ms);

  /**
   * @brief Build a 0x3D answering @p challenge for @p request_data
   *
   * The low-level builder behind answer_challenge(): the payload is
   * create_2w_hmac(request_data, challenge, key), the frame is plain. Callers
   * must only use it for a request they actually sent - see the class note.
   *
   * @param request_data Command id + parameters of the request being authenticated
   */
  bool create_challenge_response(
    frame::IoFrame* frame,
    const uint8_t dest_node[NODE_ID_SIZE],
    const uint8_t src_node[NODE_ID_SIZE],
    const uint8_t challenge[HMAC_SIZE],
    const uint8_t* request_data,
    size_t request_len
  );

  // --------------------------------------------------------------------------
  // Pairing
  // --------------------------------------------------------------------------

  /**
   * @brief Recover a system key pushed to us in a 2W key transfer (0x32)
   *
   * The counterpart to sending a key. When this node has answered a peer's
   * "ask challenge" (0x31) with a challenge of its own (0x3C, via
   * create_challenge_request(), which stores the nonce), the peer replies with
   * a 0x32 carrying its key masked against an IV built from that nonce and the
   * frame that requested the transfer. This undoes the mask.
   *
   * @param key_frame The received 0x32 frame
   * @param request_frame_data The frame that seeded the IV: for the documented
   *        push that is the "ask challenge" command byte, {0x31}
   * @param request_data_len Length of @p request_frame_data
   * @param key_out Recovered key (16 bytes)
   * @return true if the frame was a 0x32 of the right size and unmasking ran
   */
  bool recover_2w_key(const frame::IoFrame* key_frame,
                      const uint8_t* request_frame_data, size_t request_data_len,
                      uint8_t key_out[AES_KEY_SIZE]) const;

  /**
   * @brief Get the current challenge (6 bytes)
   *
   * Only meaningful while has_active_challenge() is true; zeroed otherwise.
   */
  const uint8_t* get_current_challenge() const { return current_challenge_; }

  /**
   * @brief Whether a challenge we sent is still waiting for its answer
   *
   * Expires the state as a side effect, like get_state().
   */
  bool has_active_challenge(unsigned long now_ms);

  /**
   * @brief Get the challenger state, expiring it if it has timed out
   */
  ChallengeState get_state(unsigned long now_ms);

  /**
   * @brief Get the last known challenger state without expiring it
   */
  ChallengeState peek_state() const { return state_; }

  /**
   * @brief Whether a peer recently proved the key (a 0x3D verified)
   */
  bool is_authenticated(unsigned long now_ms) { return get_state(now_ms) == ChallengeState::AUTHENTICATED; }

  /**
   * @brief Whether the last verified 0x3D came from this peer, recently
   */
  bool is_authenticated_with(const uint8_t peer[NODE_ID_SIZE], unsigned long now_ms);

  /**
   * @brief Reset the challenger state and wipe the stored challenge
   *
   * The initiator's pending request is left alone; see clear_pending_request().
   */
  void reset();

  void set_challenge_timeout_ms(uint32_t timeout_ms) { challenge_timeout_ms_ = timeout_ms; }
  void set_session_timeout_ms(uint32_t timeout_ms) { session_timeout_ms_ = timeout_ms; }
  void set_response_window_ms(uint32_t window_ms) { response_window_ms_ = window_ms; }

protected:
  uint8_t system_key_[AES_KEY_SIZE];
  bool key_set_;

  // Challenger
  uint8_t current_challenge_[HMAC_SIZE];
  /// Who we challenged, and as whom - only a response between these two counts.
  uint8_t peer_node_[NODE_ID_SIZE];
  uint8_t own_node_[NODE_ID_SIZE];
  bool peer_known_;
  ChallengeState state_;
  unsigned long state_timestamp_ms_;
  uint32_t challenge_timeout_ms_;
  uint32_t session_timeout_ms_;
  /// The peer's command the pending challenge authenticates.
  frame::IoFrame held_request_;
  bool held_valid_;
  bool verified_ready_;

  // Initiator
  uint8_t pending_request_[MAX_REQUEST_SIZE];
  size_t pending_len_;
  uint8_t pending_dest_[NODE_ID_SIZE];
  uint8_t pending_src_[NODE_ID_SIZE];
  unsigned long pending_ts_ms_;
  uint8_t pending_answers_;
  bool pending_valid_;
  uint32_t response_window_ms_;
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
   * @param device_type Type of device to discover: the 10-bit type half of the
   *        node type field, i.e. node_type_of() of a NodeType (2 for a roller
   *        shutter, 4 for a window opener, 0x3FF for a remote controller), or
   *        DISCOVER_ALL_TYPES (0xFF) for all. Because 0xFF has always meant
   *        "all", the one type with that index (CENTRAL_HOUSE_CONTROL) cannot
   *        be singled out.
   * @param timeout_ms Discovery timeout in milliseconds
   * @param now_ms Current time in milliseconds
   */
  void start_discovery(uint16_t device_type, unsigned long timeout_ms, unsigned long now_ms);

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
  bool create_discovery_request(frame::IoFrame* frame, uint16_t device_type);

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
  uint16_t discovery_device_type_;  // 10-bit type index, or DISCOVER_ALL_TYPES

  DiscoveredDevice discovered_devices_[MAX_DISCOVERED_DEVICES];
  size_t discovered_count_;
};

} // namespace mode2w
} // namespace iohome
