/**
 * @file IoHomeControl.h
 * @brief io-homecontrol Node Controller
 * @author iown-homecontrol project
 *
 * High-level controller for io-homecontrol devices supporting both
 * 1W and 2W modes with RadioLib integration.
 */

#pragma once

#include <RadioLib.h>
#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"
#include "protocol/iohome_frame.h"
#include "protocol/iohome_2w.h"

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
 * @brief io-homecontrol Node Controller
 *
 * This class provides a high-level interface for controlling io-homecontrol
 * devices. It handles RadioLib communication, encryption, and protocol details.
 */
class IoHomeControl {
public:
  /**
   * @brief Construct a new IoHomeControl object
   *
   * @param radio Pointer to RadioLib PhysicalLayer (e.g., SX1276, RFM69, Si4463)
   */
  IoHomeControl(PhysicalLayer* radio);

  /**
   * @brief Initialize the controller
   *
   * @param own_node_id This controller's node ID (3 bytes)
   * @param system_key System key for encryption (16 bytes)
   * @param is_1w true for 1W mode, false for 2W mode
   * @return true on success, false on error
   */
  bool begin(const uint8_t own_node_id[NODE_ID_SIZE],
             const uint8_t system_key[AES_KEY_SIZE],
             bool is_1w = true);

  /**
   * @brief Configure physical layer parameters
   *
   * Sets frequency, modulation, data rate, etc.
   *
   * @param frequency Center frequency in MHz (default: 868.95)
   * @return 0 on success, error code otherwise
   */
  int16_t configure_radio(float frequency = FREQUENCY_CHANNEL_2);

  /**
   * @brief Start receiving frames
   *
   * @param callback Function to call when frame is received (can be nullptr)
   * @return 0 on success, error code otherwise
   */
  int16_t start_receive(FrameReceivedCallback callback = nullptr);

  /**
   * @brief Stop receiving frames
   */
  void stop_receive();

  /**
   * @brief Check for received frames (polling mode)
   *
   * Call this regularly in loop() when not using callbacks.
   *
   * @param frame Output IoFrame structure
   * @param rssi Output RSSI value
   * @param snr Output SNR value
   * @return true if frame was received, false otherwise
   */
  bool check_received(frame::IoFrame* frame, int16_t* rssi = nullptr, float* snr = nullptr);

  /**
   * @brief Send a command to a device
   *
   * @param dest_node Destination node ID (3 bytes)
   * @param cmd_id Command ID
   * @param params Command parameters (can be nullptr)
   * @param params_len Length of parameters
   * @return true on success, false on error
   */
  bool send_command(const uint8_t dest_node[NODE_ID_SIZE],
                    uint8_t cmd_id,
                    const uint8_t* params = nullptr,
                    size_t params_len = 0);

  /**
   * @brief Set position of an actuator (e.g., blind, shutter)
   *
   * @param dest_node Destination node ID (3 bytes)
   * @param position Position value (0-100%)
   * @return true on success, false on error
   */
  bool set_position(const uint8_t dest_node[NODE_ID_SIZE], uint8_t position);

  /**
   * @brief Open an actuator (100%)
   *
   * @param dest_node Destination node ID (3 bytes)
   * @return true on success, false on error
   */
  bool open(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Close an actuator (0%)
   *
   * @param dest_node Destination node ID (3 bytes)
   * @return true on success, false on error
   */
  bool close(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Stop actuator movement
   *
   * @param dest_node Destination node ID (3 bytes)
   * @return true on success, false on error
   */
  bool stop(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Get current RSSI
   *
   * @return RSSI value in dBm
   */
  int16_t get_rssi();

  /**
   * @brief Get current SNR
   *
   * @return SNR value in dB
   */
  float get_snr();

  /**
   * @brief Get rolling code (1W mode only)
   *
   * @return Current rolling code value
   */
  uint16_t get_rolling_code() const { return rolling_code_; }

  /**
   * @brief Set rolling code (1W mode only)
   *
   * @param code Rolling code value
   */
  void set_rolling_code(uint16_t code) { rolling_code_ = code; }

  /**
   * @brief Enable/disable verbose logging
   *
   * @param enable true to enable, false to disable
   */
  void set_verbose(bool enable) { verbose_ = enable; }

  // ========================================================================
  // 2W Mode Features
  // ========================================================================

  /**
   * @brief Enable frequency hopping (2W mode only)
   *
   * @param enable true to enable, false to disable
   * @return true on success
   */
  bool enable_frequency_hopping(bool enable = true);

  /**
   * @brief Update frequency hopping state (call in loop for 2W mode)
   *
   * @return true if channel was switched
   */
  bool update_frequency_hopping();

  /**
   * @brief Get current channel (2W mode)
   *
   * @return Current ChannelState
   */
  mode2w::ChannelState get_current_channel() const;

  /**
   * @brief Send challenge request (2W mode)
   *
   * @param dest_node Destination node ID (3 bytes)
   * @return true on success
   */
  bool send_challenge_request(const uint8_t dest_node[NODE_ID_SIZE]);

  /**
   * @brief Send challenge response (2W mode)
   *
   * @param dest_node Destination node ID (3 bytes)
   * @param challenge Challenge from request (6 bytes)
   * @return true on success
   */
  bool send_challenge_response(const uint8_t dest_node[NODE_ID_SIZE], const uint8_t challenge[HMAC_SIZE]);

  /**
   * @brief Get authentication state (2W mode)
   *
   * @return Current ChallengeState
   */
  mode2w::ChallengeState get_auth_state() const;

  /**
   * @brief Start device discovery
   *
   * @param device_type Type of device to discover (0xFF for all)
   * @param timeout_ms Discovery timeout in milliseconds
   */
  void start_discovery(uint8_t device_type = 0xFF, unsigned long timeout_ms = 10000);

  /**
   * @brief Stop device discovery
   */
  void stop_discovery();

  /**
   * @brief Get number of discovered devices
   *
   * @return Number of devices found
   */
  size_t get_discovered_count() const;

  /**
   * @brief Get discovered device by index
   *
   * @param index Device index (0 to count-1)
   * @param device Output DiscoveredDevice structure
   * @return true if device exists
   */
  bool get_discovered_device(size_t index, mode2w::DiscoveredDevice* device);

  /**
   * @brief Pair device with key transfer (1W mode)
   *
   * @param dest_node Target device node ID (3 bytes)
   * @param new_system_key System key to program (16 bytes)
   * @return true on success
   */
  bool pair_device_1w(const uint8_t dest_node[NODE_ID_SIZE], const uint8_t new_system_key[AES_KEY_SIZE]);

  /**
   * @brief Pair device with key transfer (2W mode)
   *
   * @param dest_node Target device node ID (3 bytes)
   * @param new_system_key System key to program (16 bytes)
   * @return true on success
   */
  bool pair_device_2w(const uint8_t dest_node[NODE_ID_SIZE], const uint8_t new_system_key[AES_KEY_SIZE]);

  /**
   * @brief Check if last beacon was recent (2W mode)
   *
   * @param timeout_ms Timeout in milliseconds
   * @return true if recent beacon exists
   */
  bool has_recent_beacon(unsigned long timeout_ms = 5000);

  /**
   * @brief Get last beacon information (2W mode)
   *
   * @param info Output BeaconInfo structure
   * @return true if beacon info is available
   */
  bool get_last_beacon(mode2w::BeaconInfo* info);

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

  // 2W Mode Components
  mode2w::ChannelHopper* channel_hopper_;
  mode2w::AuthenticationManager* auth_manager_;
  mode2w::BeaconHandler* beacon_handler_;
  mode2w::DiscoveryManager* discovery_manager_;

  /**
   * @brief Transmit a frame
   *
   * @param frame Pointer to IoFrame to transmit
   * @return true on success, false on error
   */
  bool transmit_frame(const frame::IoFrame* frame);

  /**
   * @brief Log message (if verbose enabled)
   *
   * @param message Message to log
   */
  void log(const char* message);

  /**
   * @brief Process received frame (internal)
   *
   * @param frame Received frame
   * @param rssi RSSI value
   * @param snr SNR value
   */
  void process_received_frame(const frame::IoFrame* frame, int16_t rssi, float snr);
};

} // namespace iohome
