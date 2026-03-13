/**
 * @file iohome_velux.h
 * @brief Velux-specific features and commands
 * @author iown-homecontrol project
 *
 * Specialized support for Velux roof windows (Dachfenster) including:
 * - Predefined ventilation positions
 * - Rain sensor integration
 * - Window specific commands
 * - Model-specific configurations
 */

#pragma once

#include <stdint.h>
#include "../protocol/iohome_constants.h"
#include "../protocol/iohome_frame.h"

namespace iohome {
namespace velux {

// ============================================================================
// Velux-Specific Constants
// ============================================================================

/**
 * @brief Velux window positions (standardized)
 */
enum class WindowPosition : uint8_t {
  CLOSED = 0,              // 0% - Completely closed
  VENTILATION_1 = 10,      // 10% - Minimal ventilation (Lüftungsstellung 1)
  VENTILATION_2 = 20,      // 20% - Medium ventilation (Lüftungsstellung 2)
  VENTILATION_3 = 30,      // 30% - Maximum ventilation (Lüftungsstellung 3)
  HALF_OPEN = 50,          // 50% - Half open
  FULLY_OPEN = 100         // 100% - Fully open
};

/**
 * @brief Velux device models
 */
enum class VeluxModel : uint8_t {
  UNKNOWN = 0x00,

  // Roof Windows (Dachfenster)
  GGL = 0x01,              // GGL - Top-operated roof window
  GGU = 0x02,              // GGU - Top-operated roof window
  GPL = 0x03,              // GPL - Top-operated roof window
  GPU = 0x04,              // GPU - Top-operated roof window

  // Solar Windows
  GGL_SOLAR = 0x11,        // GGL with solar panel
  GGU_SOLAR = 0x12,        // GGU with solar panel

  // Electric Windows
  GGL_ELECTRIC = 0x21,     // GGL with electric motor (KMX 100)
  GGU_ELECTRIC = 0x22,     // GGU with electric motor (KMX 200)

  // Blinds
  DML = 0x31,              // DML - Blackout blind
  RML = 0x32,              // RML - Roller blind
  FML = 0x33,              // FML - Pleated blind
  MML = 0x34,              // MML - Awning blind (outside)
  SML = 0x35,              // SML - Roller shutter (outside)

  // Controllers
  KLR_200 = 0x41,          // KLR 200 - Remote control pad
  KLI_310 = 0x42,          // KLI 310 - Wall switch
  KLF_200 = 0x43           // KLF 200 - Internet gateway
};

/**
 * @brief Rain sensor status
 */
enum class RainSensorStatus : uint8_t {
  UNKNOWN = 0x00,
  DRY = 0x01,              // No rain detected
  RAIN = 0x02,             // Rain detected
  ERROR = 0xFF             // Sensor error
};

/**
 * @brief Velux-specific commands (in addition to standard commands)
 */
constexpr uint8_t VELUX_CMD_GET_RAIN_SENSOR = 0x58;     // Get rain sensor status
constexpr uint8_t VELUX_CMD_SET_VENTILATION = 0x59;     // Set ventilation mode
constexpr uint8_t VELUX_CMD_EMERGENCY_CLOSE = 0x5A;     // Emergency close (rain)
constexpr uint8_t VELUX_CMD_GET_WINDOW_STATUS = 0x5B;   // Extended status
constexpr uint8_t VELUX_CMD_RESET_LIMITS = 0x5C;        // Reset position limits
constexpr uint8_t VELUX_CMD_SET_LIMITS = 0x5D;          // Set position limits

// ============================================================================
// Velux Window Controller
// ============================================================================

/**
 * @brief Velux Window-specific controller
 *
 * Provides high-level functions for Velux roof windows with
 * predefined positions and rain sensor integration.
 */
class VeluxWindow {
public:
  /**
   * @brief Construct Velux window controller
   *
   * @param node_id Window's node ID (3 bytes)
   * @param model Window model
   */
  VeluxWindow(const uint8_t node_id[NODE_ID_SIZE], VeluxModel model = VeluxModel::UNKNOWN);

  /**
   * @brief Set window to ventilation position
   *
   * @param level Ventilation level (1-3)
   * @return Position value (0-100)
   */
  uint8_t get_ventilation_position(uint8_t level) const;

  /**
   * @brief Create frame for ventilation position
   *
   * @param frame Output IoFrame
   * @param src_node Source node ID (3 bytes)
   * @param level Ventilation level (1-3)
   * @return true on success
   */
  bool create_ventilation_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE],
    uint8_t level
  );

  /**
   * @brief Create frame for specific window position
   *
   * @param frame Output IoFrame
   * @param src_node Source node ID (3 bytes)
   * @param position Window position enum
   * @return true on success
   */
  bool create_position_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE],
    WindowPosition position
  );

  /**
   * @brief Create emergency close frame (for rain)
   *
   * @param frame Output IoFrame
   * @param src_node Source node ID (3 bytes)
   * @return true on success
   */
  bool create_emergency_close_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE]
  );

  /**
   * @brief Parse rain sensor status from frame
   *
   * @param frame Received frame
   * @return Rain sensor status
   */
  static RainSensorStatus parse_rain_sensor_status(const frame::IoFrame* frame);

  /**
   * @brief Get node ID
   *
   * @return Pointer to node ID (3 bytes)
   */
  const uint8_t* get_node_id() const { return node_id_; }

  /**
   * @brief Get window model
   *
   * @return VeluxModel
   */
  VeluxModel get_model() const { return model_; }

  /**
   * @brief Set rain protection enabled
   *
   * @param enabled true to enable automatic closing on rain
   */
  void set_rain_protection(bool enabled) { rain_protection_enabled_ = enabled; }

  /**
   * @brief Check if rain protection is enabled
   *
   * @return true if enabled
   */
  bool is_rain_protection_enabled() const { return rain_protection_enabled_; }

protected:
  uint8_t node_id_[NODE_ID_SIZE];
  VeluxModel model_;
  bool rain_protection_enabled_;
  RainSensorStatus last_rain_status_;
};

// ============================================================================
// Velux Blind Controller
// ============================================================================

/**
 * @brief Velux Blind-specific controller
 *
 * For DML, RML, FML, MML, SML blinds.
 */
class VeluxBlind {
public:
  /**
   * @brief Construct Velux blind controller
   *
   * @param node_id Blind's node ID (3 bytes)
   * @param model Blind model
   */
  VeluxBlind(const uint8_t node_id[NODE_ID_SIZE], VeluxModel model);

  /**
   * @brief Get recommended positions for blind type
   *
   * Different blind types have different useful positions.
   *
   * @param positions Output array (max 5 positions)
   * @return Number of positions
   */
  size_t get_recommended_positions(uint8_t positions[5]) const;

  /**
   * @brief Check if blind supports tilt
   *
   * @return true if tilt is supported
   */
  bool supports_tilt() const;

  /**
   * @brief Create tilt frame (for venetian blinds)
   *
   * @param frame Output IoFrame
   * @param src_node Source node ID (3 bytes)
   * @param tilt_angle Tilt angle (0-100)
   * @return true on success
   */
  bool create_tilt_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE],
    uint8_t tilt_angle
  );

  const uint8_t* get_node_id() const { return node_id_; }
  VeluxModel get_model() const { return model_; }

protected:
  uint8_t node_id_[NODE_ID_SIZE];
  VeluxModel model_;
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Detect Velux model from device type and manufacturer code
 *
 * @param device_type Device type from discovery
 * @param manufacturer Manufacturer code
 * @return Detected VeluxModel
 */
VeluxModel detect_model(uint8_t device_type, uint8_t manufacturer);

/**
 * @brief Get human-readable model name
 *
 * @param model VeluxModel
 * @return Model name string
 */
const char* get_model_name(VeluxModel model);

/**
 * @brief Check if model is a roof window
 *
 * @param model VeluxModel
 * @return true if roof window
 */
bool is_roof_window(VeluxModel model);

/**
 * @brief Check if model is a blind
 *
 * @param model VeluxModel
 * @return true if blind
 */
bool is_blind(VeluxModel model);

/**
 * @brief Check if model supports rain sensor
 *
 * @param model VeluxModel
 * @return true if rain sensor supported
 */
bool supports_rain_sensor(VeluxModel model);

/**
 * @brief Get optimal ventilation position for current temperature
 *
 * Recommends ventilation level based on indoor temperature.
 *
 * @param indoor_temp_celsius Indoor temperature in Celsius
 * @return Recommended ventilation level (0-3, 0 = close)
 */
uint8_t get_recommended_ventilation(float indoor_temp_celsius);

} // namespace velux
} // namespace iohome
