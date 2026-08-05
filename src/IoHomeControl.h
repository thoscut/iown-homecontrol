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
   * @brief Pair a device by transferring a key (2W mode)
   */
  bool pair_device_2w(const uint8_t dest_node[NODE_ID_SIZE],
                      const uint8_t new_system_key[AES_KEY_SIZE]);

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
