/**
 * @file IoHomeControl.h
 * @brief io-homecontrol Node Controller
 * @author iown-homecontrol project
 *
 * High-level controller for io-homecontrol devices supporting both
 * 1W and 2W modes with RadioLib integration.
 *
 * @note RadioLib's FSK modem defaults do not match io-homecontrol. Besides the
 *       parameters configure_radio() sets through PhysicalLayer, the concrete
 *       radio class must also be told to
 *         - disable the radio's own CRC (io-homecontrol appends its own), and
 *         - use fixed packet length without RadioLib's length byte,
 *       because those calls are not part of the PhysicalLayer interface.
 *       See docs/RADIO-SETUP.md for per-chip snippets.
 */

#pragma once

#include <RadioLib.h>
#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"
#include "protocol/iohome_frame.h"
#include "protocol/iohome_phy_framing.h"
#include "protocol/iohome_2w.h"
#include "protocol/iohome_replay_guard.h"
#include "protocol/iohome_rolling_code_store.h"

namespace iohome {

/**
 * @brief Callback function type for received frames
 *
 * @param frame Pointer to received IoFrame
 * @param rssi RSSI value in dBm
 * @param snr SNR value in dB
 */
typedef void (*FrameReceivedCallback)(const frame::IoFrame* frame, int16_t rssi, float snr);

/**
 * @brief Callback used to program the radio's fixed payload length
 *
 * RadioLib's fixed-length FSK mode transmits exactly the programmed number of
 * bytes, but io-homecontrol frames vary between 11 and 34 bytes. The call that
 * changes it lives on the concrete radio class (SX1276, SX1262, ...) and is not
 * part of PhysicalLayer, so the controller asks the application to make it.
 *
 * @param length Payload length to program
 * @param context Opaque pointer supplied with the callback
 * @return RADIOLIB_ERR_NONE on success
 */
typedef int16_t (*PacketLengthCallback)(uint8_t length, void* context);

/**
 * @brief Callback invoked for every packet the radio hands over, unvalidated
 *
 * The normal receive path drops anything that fails its CRC, its MAC or the
 * replay check, and rightly so. But those are exactly the frames worth looking
 * at when reverse-engineering: a frame from an unpaired actuator, a command
 * this library does not model yet, a capture that disagrees with docs/.
 *
 * This hook fires before any of that, straight after readData() succeeds, so a
 * sniffer sees the bytes as they arrived. It does not affect what the receive
 * path then does with them.
 *
 * Called from check_received(), not from the ISR, so it may print.
 *
 * @param data Frame bytes as delivered by the radio
 * @param len Number of bytes
 * @param rssi Received signal strength, dBm
 * @param snr Signal-to-noise ratio, dB
 * @param context Opaque pointer supplied with the callback
 */
typedef void (*RawFrameCallback)(const uint8_t* data, size_t len, int16_t rssi, float snr,
                                 void* context);

/**
 * @brief Severity of a library log message
 */
enum class LogLevel : uint8_t {
  ERROR = 0,   // Something failed and the caller's request did not happen
  WARN,        // Something unexpected, but the library carried on
  INFO,        // Normal progress worth recording
  DEBUG        // Detail only useful while diagnosing
};

/**
 * @brief Sink for the library's log messages
 *
 * Without one, messages go to `Serial` on Arduino and `stdout` elsewhere, and
 * only when set_verbose(true) is on. That is fine for a sketch and no use at
 * all inside a larger application: ESPHome has its own logger with its own
 * levels and tags, a host test wants to assert on what was logged, and neither
 * can do anything with a bare printf.
 *
 * Install a callback and every message arrives with its severity attached, for
 * the application to route, filter or drop. Messages are already formatted;
 * the callback receives a NUL-terminated string it does not own.
 *
 * Called from the same context as the operation that logged it - never from
 * the packet ISR.
 *
 * @param level Severity
 * @param message Formatted message, without a trailing newline
 * @param context Opaque pointer supplied with the callback
 */
typedef void (*LogCallback)(LogLevel level, const char* message, void* context);

/**
 * @brief Sink for a system key received during 2W pairing
 *
 * Fired when this node, acting as the follower in a 2W key transfer, has
 * recovered and adopted a key pushed to it by a controller. The key is already
 * in effect (set_system_key() has run); this is the hook to persist it so it
 * survives a reboot.
 *
 * Fires whenever this node obtains a key over the air: as a follower being
 * pushed one (which it has already adopted, set_system_key() has run), or as a
 * controller pulling a device's existing key (which it has *not* adopted - that
 * is the peer's key, surfaced so the application can store it against that
 * peer). @p from_node tells the two apart: it is the sender of the key.
 *
 * @param key The 16-byte key obtained
 * @param from_node The node it came from (3 bytes)
 * @param context Opaque pointer supplied with the callback
 */
typedef void (*KeyReceivedCallback)(const uint8_t key[16], const uint8_t from_node[3],
                                    void* context);

/**
 * @brief State of a 2W pairing exchange in progress
 */
enum class Pairing2W : uint8_t {
  IDLE = 0,
  PUSH_WAIT_CHALLENGE,  // controller sent 0x31, waiting for the device's 0x3C
  PUSH_WAIT_ACK,        // controller sent 0x32, waiting for the device's 0x33
  PULL_WAIT_KEY         // controller sent 0x38, waiting for the device's 0x32
};

/**
 * @brief Why a received frame was discarded
 */
enum class RxReject : uint8_t {
  NONE = 0,
  RADIO_ERROR,      // readData() failed
  MALFORMED,        // parse_frame() rejected the buffer
  CRC,              // CRC mismatch
  MAC,              // MAC verification failed
  REPLAY,           // Rolling code did not move forward
  UNAUTHENTICATED   // Plain frame received while authentication is required
};

/**
 * @brief Receive-path statistics, useful for diagnosing a noisy link
 */
struct RxStats {
  /// Packets the radio delivered, whatever became of them afterwards.
  uint32_t received;
  uint32_t accepted;
  uint32_t radio_errors;
  uint32_t malformed;
  uint32_t crc_failures;
  uint32_t mac_failures;
  uint32_t replays;
  uint32_t unauthenticated;
};

/**
 * @brief io-homecontrol Node Controller
 *
 * This class provides a high-level interface for controlling io-homecontrol
 * devices. It handles RadioLib communication, authentication and protocol
 * details.
 *
 * @note Reception is interrupt driven. Because RadioLib's packet callback is a
 *       plain function pointer with no user context, a single instance can own
 *       the interrupt at a time - which matches the one-radio-per-board reality.
 *       Drive check_received() from loop().
 */
class IoHomeControl {
public:
  /**
   * @brief Construct a new IoHomeControl object
   *
   * @param radio Pointer to RadioLib PhysicalLayer (e.g., SX1276, SX1262)
   */
  explicit IoHomeControl(PhysicalLayer* radio);

  /**
   * @brief Destroy the IoHomeControl object
   *
   * Cleans up dynamically allocated 2W mode components and wipes the key.
   */
  ~IoHomeControl();

  // Non-copyable: owns raw pointers and the packet interrupt.
  IoHomeControl(const IoHomeControl&) = delete;
  IoHomeControl& operator=(const IoHomeControl&) = delete;

  /**
   * @brief Initialize the controller
   *
   * @param own_node_id This controller's node ID (3 bytes)
   * @param system_key System key for authentication (16 bytes)
   * @param is_1w true for 1W mode, false for 2W mode
   * @return true on success, false on error
   */
  bool begin(const uint8_t own_node_id[NODE_ID_SIZE],
             const uint8_t system_key[AES_KEY_SIZE],
             bool is_1w = true);

  /**
   * @brief Configure physical layer parameters
   *
   * Sets frequency, modulation, data rate, sync word and preamble.
   *
   * @param frequency Center frequency in MHz (default: 868.95)
   * @return RADIOLIB_ERR_NONE on success, error code otherwise
   */
  int16_t configure_radio(float frequency = FREQUENCY_CHANNEL_2);

  /**
   * @brief Start receiving frames
   *
   * Registers the packet interrupt and puts the radio into receive mode.
   *
   * @param callback Function to call when a frame is accepted (may be nullptr)
   * @return RADIOLIB_ERR_NONE on success, error code otherwise
   */
  int16_t start_receive(FrameReceivedCallback callback = nullptr);

  /**
   * @brief Stop receiving frames
   */
  void stop_receive();

  /**
   * @brief Check for received frames
   *
   * Call this regularly from loop(). Returns true once per accepted frame.
   *
   * @param frame Output IoFrame structure
   * @param rssi Output RSSI value (optional)
   * @param snr Output SNR value (optional)
   * @return true if a valid frame was received
   */
  bool check_received(frame::IoFrame* frame, int16_t* rssi = nullptr, float* snr = nullptr);

  /**
   * @brief Signal that a packet is waiting
   *
   * Call this from your own ISR if you manage the radio interrupt yourself.
   */
  static void notify_packet_received();

  /**
   * @brief Reason the most recent frame was discarded
   */
  RxReject last_reject_reason() const { return last_reject_; }

  /**
   * @brief Receive-path statistics
   */
  const RxStats& rx_stats() const { return rx_stats_; }

  /**
   * @brief Reset receive-path statistics
   */
  void reset_rx_stats();

  /**
   * @brief Send a raw command to a device
   *
   * @param dest_node Destination node ID (3 bytes)
   * @param cmd_id Command ID
   * @param params Command parameters (may be nullptr)
   * @param params_len Length of parameters
   * @return true on success, false on error
   */
  bool send_command(const uint8_t dest_node[NODE_ID_SIZE],
                    uint8_t cmd_id,
                    const uint8_t* params = nullptr,
                    size_t params_len = 0);

  /**
   * @brief Send an "Activate/Execute Function" command (0x00)
   *
   * This is how actuators are actually driven: a Main Parameter carried by
   * command 0x00, not a dedicated per-action command ID.
   *
   * @param dest_node Destination node ID (3 bytes)
   * @param main_param Main parameter (MP_OPEN, MP_CLOSE, MP_STOP, or a
   *                   percentage produced by mp_from_percent_closed())
   * @param fp1 Functional parameter 1
   * @param fp2 Functional parameter 2
   * @return true on success, false on error
   */
  bool send_execute(const uint8_t dest_node[NODE_ID_SIZE],
                    uint16_t main_param,
                    uint8_t fp1 = 0x00,
                    uint8_t fp2 = 0x00);

  /**
   * @brief Send command 0x00 with an arbitrary number of functional parameters
   *
   * Most actuators only need FP1 and FP2, which send_execute() covers. Some
   * carry more - the capture in scripts/io-homecontrol.ksy has four - and
   * multi-channel actuator types address their channels through the extra
   * parameters.
   *
   * @param dest_node  Destination node ID (3 bytes)
   * @param main_param Main parameter
   * @param fps        Functional parameters
   * @param fp_count   Number of functional parameters, 2 to
   *                   EXECUTE_MAX_FUNCTIONAL_PARAMS
   * @return true on success, false on error
   */
  bool send_execute_fp(const uint8_t dest_node[NODE_ID_SIZE],
                       uint16_t main_param,
                       const uint8_t* fps,
                       size_t fp_count);

  /**
   * @brief Move an actuator to a position
   *
   * @param dest_node Destination node ID (3 bytes)
   * @param percent_open 0 = fully closed, 100 = fully open
   * @return true on success, false on error
   */
  bool set_position(const uint8_t dest_node[NODE_ID_SIZE], uint8_t percent_open);

  /**
   * @brief Open an actuator fully
   */
  bool open(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Close an actuator fully
   */
  bool close(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Stop actuator movement
   */
  bool stop(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Move a window to its secured ventilation position
   *
   * The window opens far enough to air the room while staying locked. This is
   * the "airing" position a Velux roof window (including the solar GGL/GGU
   * models) offers - Main Parameter 0xD803, the window opener actuator profile's
   * secured-ventilation alias. Sending it to a product that is not a window
   * opener has no defined meaning.
   */
  bool ventilate(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Send a Velux remote's "force" preset (Main Parameter 0x6400)
   *
   * A fixed preset a real Velux remote's dedicated button sends. Observed on
   * air rather than derived from the specification - see MP_FORCE.
   */
  bool force(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Set the command originator reported in execute frames
   */
  void set_originator(Originator originator) { originator_ = originator; }

  /**
   * @brief Set the ACEI byte used in execute frames
   *
   * @return false if bit 0 (IsValid) is clear; actuators would reject the frame
   */
  bool set_acei(uint8_t acei);

  /**
   * @brief Set the ACEI priority level, keeping the rest of the byte intact
   *
   * Only the priority level (bits 7-5) changes - the field that decides whether
   * a command outranks another. Priority service, extended info and the IsValid
   * bit are left as they were, so this composes with set_acei().
   *
   * Real remotes differ here and both are valid: our own capture and the KLF 200
   * default sit at USER_LEVEL_2 (level 3, "Default"), while the remote
   * rspaargaren/iohomecontrol emulates runs one step up at USER_LEVEL_1
   * (level 2, "High") - the level in the ACEI 0x43 that docs/commands.md records
   * from a real frame. This is the clean way to match that level. Note that
   * rspaargaren's exact 0x43 also carries Extended Info = 1, which is not part of
   * the priority; to reproduce the byte verbatim use set_acei(0x43) instead.
   */
  void set_priority(PriorityLevel level) {
    acei_ = static_cast<uint8_t>(
        (acei_ & ~ACEI_LEVEL_MASK) |
        ((static_cast<uint8_t>(level) << ACEI_LEVEL_SHIFT) & ACEI_LEVEL_MASK));
  }

  /**
   * @brief Get current RSSI, or 0 if no radio is attached
   */
  int16_t get_rssi();

  /**
   * @brief Get current SNR, or 0 if no radio is attached
   */
  float get_snr();

  /**
   * @brief Get the rolling code that will be used for the next transmission
   */
  uint16_t get_rolling_code() const { return rolling_code_; }

  /**
   * @brief Override the rolling code (1W mode)
   *
   * Moving the counter backwards makes receivers reject frames until it
   * catches up again; only do this when you know the peer's state.
   */
  void set_rolling_code(uint16_t code);

  /**
   * @brief Enable/disable verbose logging
   */
  void set_verbose(bool enable) { verbose_ = enable; }

  /**
   * @brief Set the rolling code store used for persistence
   *
   * Ownership is NOT transferred. Call before begin() so the stored counter is
   * picked up during initialization.
   *
   * Writes are batched: the controller reserves a block of counter values,
   * persists the end of that block, and only writes again once the block is
   * used up. A reboot therefore resumes past every code that could have been
   * transmitted, without writing to flash on every command.
   *
   * @param store Store implementation, or nullptr to disable persistence
   * @param reserve_block Number of codes reserved per flash write (min 1)
   */
  void set_rolling_code_store(RollingCodeStore* store, uint16_t reserve_block = 64);

  /**
   * @brief Whether frames without a MAC are accepted
   *
   * Defaults to false. Bootstrap commands that the protocol defines as
   * unauthenticated (discovery, key transfer, their acks) are always accepted.
   */
  void set_accept_plain_frames(bool accept) { accept_plain_frames_ = accept; }

  /**
   * @brief Install the fixed-payload-length hook
   *
   * When set, the controller narrows the radio to the exact frame length
   * before each transmission and widens it back to FRAME_MAX_SIZE afterwards,
   * so fixed-length FSK mode does not pad or truncate frames. Example:
   *
   * @code
   * controller.set_packet_length_callback(
   *   [](uint8_t len, void* ctx) -> int16_t {
   *     return static_cast<SX1276*>(ctx)->fixedPacketLengthMode(len);
   *   },
   *   &radio);
   * @endcode
   *
   * @param callback Hook, or nullptr to disable
   * @param context Passed back to the hook unchanged
   */
  void set_packet_length_callback(PacketLengthCallback callback, void* context = nullptr);

  /**
   * @brief Install a sniffer that sees every packet before validation
   *
   * Use this to capture traffic from actuators that are not paired with this
   * controller, or frames the protocol layer rejects. See RawFrameCallback.
   *
   * @param callback Hook, or nullptr to disable
   * @param context Passed back to the hook unchanged
   */
  void set_raw_frame_callback(RawFrameCallback callback, void* context = nullptr);

  /**
   * @brief Route log messages to the application instead of the serial port
   *
   * See LogCallback. Installing a sink also turns logging on: the verbose flag
   * only governs the built-in serial output, so a caller that wants the
   * messages does not have to know that.
   *
   * @param callback Sink, or nullptr to go back to the built-in output
   * @param context Passed back to the sink unchanged
   * @param min_level Messages below this severity are dropped
   */
  void set_log_callback(LogCallback callback, void* context = nullptr,
                        LogLevel min_level = LogLevel::DEBUG);

  /**
   * @brief Install the packet-length hook for a concrete radio class
   *
   * The hand-written form of set_packet_length_callback() needs a lambda that
   * casts a `void*` back to the radio type, and names that type twice:
   *
   * @code{.cpp}
   * controller.set_packet_length_callback(
   *   [](uint8_t len, void* ctx) -> int16_t {
   *     return static_cast<SX1276*>(ctx)->fixedPacketLengthMode(len);
   *   },
   *   &radio);
   * @endcode
   *
   * Getting that cast wrong is undefined behaviour that compiles cleanly. This
   * deduces the type from the argument, so it cannot disagree with itself:
   *
   * @code{.cpp}
   * controller.use_radio_packet_length(radio);
   * @endcode
   *
   * Works with any RadioLib class exposing `fixedPacketLengthMode(uint8_t)` -
   * SX1276, SX1262 and the rest. The lambda captures nothing, so it still
   * converts to the plain function pointer the callback takes.
   *
   * @param radio The concrete radio object; must outlive this controller.
   */
  template <typename RadioT>
  void use_radio_packet_length(RadioT& radio) {
    set_packet_length_callback(
      [](uint8_t length, void* context) -> int16_t {
        return static_cast<RadioT*>(context)->fixedPacketLengthMode(length);
      },
      &radio);
  }

  /**
   * @brief Access the replay guard protecting the receive path
   */
  ReplayGuard& replay_guard() { return replay_guard_; }

  // ========================================================================
  // 2W Mode Features
  // ========================================================================

  /**
   * @brief Enable frequency hopping (2W mode only)
   */
  bool enable_frequency_hopping(bool enable = true);

  /**
   * @brief Update frequency hopping state (call from loop in 2W mode)
   *
   * @return true if the channel was switched
   */
  bool update_frequency_hopping();

  /**
   * @brief Get current channel (2W mode)
   */
  mode2w::ChannelState get_current_channel() const;

  /**
   * @brief Send a challenge request (2W mode)
   */
  bool send_challenge_request(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Send a challenge response (2W mode)
   *
   * @param challenge Challenge from the request (6 bytes)
   */
  bool send_challenge_response(const uint8_t dest_node[NODE_ID_SIZE],
                               const uint8_t challenge[HMAC_SIZE]);

  /**
   * @brief Get authentication state (2W mode)
   */
  mode2w::ChallengeState get_auth_state();

  /**
   * @brief Start device discovery
   *
   * @param device_type Type of device to discover (0xFF for all)
   * @param timeout_ms Discovery timeout in milliseconds
   * @return true if the discovery request was transmitted
   */
  bool start_discovery(uint8_t device_type = 0xFF, unsigned long timeout_ms = 10000);

  /**
   * @brief Stop device discovery
   */
  void stop_discovery();

  /**
   * @brief Number of discovered devices
   */
  size_t get_discovered_count() const;

  /**
   * @brief Copy out a discovered device by index
   */
  bool get_discovered_device(size_t index, mode2w::DiscoveredDevice* device) const;

  /**
   * @brief Pair a device by transferring a key (1W mode)
   *
   * Sends "remove 1W controller" (0x39) followed by "send 1W key" (0x30), the
   * sequence documented for 1W discovery.
   *
   * @param dest_node Target device node ID (3 bytes)
   * @param new_system_key System key to program (16 bytes)
   * @param manufacturer Manufacturer ID byte
   * @return true on success
   */
  bool pair_device_1w(const uint8_t dest_node[NODE_ID_SIZE],
                      const uint8_t new_system_key[AES_KEY_SIZE],
                      uint8_t manufacturer = 0x00);

  /**
   * @brief Push a system key to a device (2W mode, controller/initiator role)
   *
   * Runs the documented push exchange rather than firing a lone frame:
   *
   *   1. this -> device:  0x31  ask challenge
   *   2. device -> this:  0x3C  challenge (a nonce the device chose)
   *   3. this -> device:  0x32  the key, masked against that nonce
   *   4. device -> this:  0x33  key transfer acknowledged
   *
   * This call performs step 1 and returns; steps 2-4 are driven by the received
   * 0x3C and 0x33, so the caller must keep pumping the receive path (with 2W
   * frequency hopping updated) until is_pairing() goes false. On success this
   * node adopts @p new_system_key, so its later commands to the device
   * authenticate under it. Closes the gap where the old one-shot version sent
   * 0x32 with a challenge the device had never seen.
   *
   * @param dest_node Target device node ID (3 bytes)
   * @param new_system_key Key to push and then use (16 bytes)
   * @return true if the opening 0x31 was sent
   */
  bool pair_device_2w(const uint8_t dest_node[NODE_ID_SIZE],
                      const uint8_t new_system_key[AES_KEY_SIZE]);

  /**
   * @brief Pull a device's existing key (2W mode, controller/collector role)
   *
   * The other half of pairing: instead of pushing a key, collect the one a
   * device already holds, to then command it. Runs the documented pull:
   *
   *   1. this -> device:  0x38  launch key transfer, carrying a challenge
   *   2. device -> this:  0x32  its key, masked against that challenge
   *
   * The recovered key is delivered to the key-received callback with the
   * device as @c from_node. It is *not* adopted as this node's system key - it
   * belongs to the device - so the application stores it per device. As with
   * the push, the caller pumps the receive path until is_pairing() clears.
   *
   * @param dest_node Device to collect the key from (3 bytes)
   * @return true if the opening 0x38 was sent
   */
  bool pull_device_key_2w(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Accept a system key pushed by a controller (2W follower role)
   *
   * With this enabled, an incoming 0x31 is answered with a fresh 0x3C challenge,
   * and the 0x32 that follows is unmasked, adopted as the system key, and
   * acknowledged with 0x33. The recovered key is delivered to the callback set
   * with set_key_received_callback() so it can be persisted.
   *
   * @param enabled Whether to act on pairing requests
   */
  void set_accept_pairing(bool enabled);

  /**
   * @brief Whether a pairing exchange is currently in progress
   */
  bool is_pairing() const { return pairing_state_ != Pairing2W::IDLE; }

  /**
   * @brief Install the sink for a key received during pairing
   */
  void set_key_received_callback(KeyReceivedCallback callback, void* context = nullptr);

  /**
   * @brief Replace the system key in use at runtime
   *
   * Updates both the frame authentication key and the 2W authentication
   * manager. Pairing calls this itself; it is public so an application can apply
   * a persisted key after begin().
   *
   * @param key New system key (16 bytes)
   */
  void set_system_key(const uint8_t key[AES_KEY_SIZE]);

  /**
   * @brief Check whether a beacon was received recently (2W mode)
   */
  bool has_recent_beacon(unsigned long timeout_ms = 5000);

  /**
   * @brief Copy out the last beacon information (2W mode)
   */
  bool get_last_beacon(mode2w::BeaconInfo* info) const;

protected:
  PhysicalLayer* radio_;
  FrameReceivedCallback rx_callback_;

  uint8_t own_node_id_[NODE_ID_SIZE];
  uint8_t system_key_[AES_KEY_SIZE];
  bool is_1w_mode_;
  uint16_t rolling_code_;

  bool initialized_;
  bool receiving_;
  bool verbose_;
  bool accept_plain_frames_;

  Originator originator_;
  uint8_t acei_;

  PacketLengthCallback packet_length_callback_;
  void* packet_length_context_;

  RawFrameCallback raw_frame_callback_;
  void* raw_frame_context_;

  LogCallback log_callback_;
  void* log_context_;
  LogLevel log_min_level_;

  /// True when a message of this severity would reach the sink or the port.
  bool log_enabled(LogLevel level) const;

  /// Format a message and hand it to the sink, or to the built-in output.
  ///
  /// The printf attribute is what keeps the format strings honest. While
  /// logging went through a macro that forwarded to Serial.printf on one
  /// platform and printf on the other, no compiler ever checked them.
#if defined(__GNUC__)
  __attribute__((format(printf, 3, 4)))
#endif
  void emit_log(LogLevel level, const char* format, ...) const;

  // Rolling code persistence
  RollingCodeStore* rolling_code_store_;
  uint16_t rolling_code_reserved_until_;
  uint16_t rolling_code_reserve_block_;

  // Receive path
  ReplayGuard replay_guard_;
  RxReject last_reject_;
  RxStats rx_stats_;

  // 2W Mode Components
  mode2w::ChannelHopper* channel_hopper_;
  mode2w::AuthenticationManager* auth_manager_;
  mode2w::BeaconHandler* beacon_handler_;
  mode2w::DiscoveryManager* discovery_manager_;

  // 2W pairing
  KeyReceivedCallback key_received_callback_;
  void* key_received_context_;
  bool accept_pairing_;
  Pairing2W pairing_state_;
  uint8_t pairing_peer_[NODE_ID_SIZE];
  uint8_t pairing_key_[AES_KEY_SIZE];
  /// Challenge sent in our 0x38, needed to unmask the device's 0x32 (pull).
  uint8_t pairing_challenge_[HMAC_SIZE];
  /// When the current pairing wait began, so a stalled peer cannot leave us
  /// stuck. See expire_stale_pairing() / PAIRING_TIMEOUT_MS.
  uint32_t pairing_started_ms_;

  /// A pairing handshake that has not completed within this long is abandoned.
  /// Long enough for a slow device to answer, short enough that a dropped peer
  /// does not keep diverting unrelated 0x3C/0x32 frames from MAC screening.
  static constexpr uint32_t PAIRING_TIMEOUT_MS = 5000;

  /// Reset a pairing handshake to IDLE if it has been waiting too long. Called
  /// on the receive path so the guard runs even without a matching answer.
  void expire_stale_pairing();

  /// Bytes to pull off the air per packet. A frame is at most FRAME_MAX_SIZE
  /// logical bytes, each ten bits on the wire, so up to 43 arrive; 64 leaves
  /// room for that and the trailing preamble the receiver sees after it.
  static constexpr size_t RX_CAPTURE_SIZE = 64;

  /// True when a frame belongs to a pairing exchange and must skip the session
  /// authentication gate (the peers do not yet share the key that gate checks).
  bool is_pairing_frame(const frame::IoFrame* frame) const;

  /// Advance the pairing state machine on a received pairing frame.
  void handle_pairing_frame(const frame::IoFrame* frame);

  /**
   * @brief Transmit a frame
   */
  bool transmit_frame(const frame::IoFrame* frame);

  /**
   * @brief Log message (if verbose enabled)
   */
  void log(const char* message);

  /**
   * @brief Apply the receive policy to a parsed frame
   *
   * @return RxReject::NONE if the frame should be delivered
   */
  RxReject screen_frame(const frame::IoFrame* frame);

  /**
   * @brief Process an accepted frame (beacons, discovery, auth)
   */
  void process_received_frame(const frame::IoFrame* frame, int16_t rssi, float snr);

  /**
   * @brief Release the 2W components
   */
  void destroy_2w_components();

  /**
   * @brief Take the next rolling code, extending the persisted reservation
   */
  uint16_t consume_rolling_code();
};

} // namespace iohome
