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
  CLOSED = 0,              // 0% open - completely closed
  VENTILATION_1 = 10,      // 10% open - minimal ventilation (Lueftungsstellung 1)
  VENTILATION_2 = 20,      // 20% open - medium ventilation (Lueftungsstellung 2)
  VENTILATION_3 = 30,      // 30% open - maximum ventilation (Lueftungsstellung 3)
  HALF_OPEN = 50,          // 50% open
  FULLY_OPEN = 100         // 100% open
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
 * @brief Velux-specific command IDs
 *
 * @warning UNVERIFIED. These IDs are not documented in docs/commands.md and
 *          have not been confirmed against a capture. They sit in the range the
 *          standard reserves for naming/info commands (0x50-0x57 are documented
 *          there), so sending them may do something unexpected. Treat every
 *          helper that uses them as experimental.
 *
 *          Standard actuator control - opening, closing, positioning,
 *          ventilation - does *not* need these: it goes through command 0x00
 *          with a Main Parameter, which is what the helpers below emit.
 */
constexpr uint8_t VELUX_CMD_GET_RAIN_SENSOR = 0x58;     // UNVERIFIED
constexpr uint8_t VELUX_CMD_SET_VENTILATION = 0x59;     // UNVERIFIED
constexpr uint8_t VELUX_CMD_EMERGENCY_CLOSE = 0x5A;     // UNVERIFIED
constexpr uint8_t VELUX_CMD_GET_WINDOW_STATUS = 0x5B;   // UNVERIFIED
constexpr uint8_t VELUX_CMD_RESET_LIMITS = 0x5C;        // UNVERIFIED
constexpr uint8_t VELUX_CMD_SET_LIMITS = 0x5D;          // UNVERIFIED

// ============================================================================
// Velux Window Controller
// ============================================================================

/**
 * @brief Velux Window-specific controller
 *
 * Provides high-level functions for Velux roof windows with
 * predefined positions and rain sensor integration.
 *
 * @note The create_*_frame() helpers return frames that are addressed and
 *       filled in but *not* finalized. The caller owns the system key (1W) or
 *       the challenge (2W) and must call frame::finalize_frame() - or
 *       frame::finalize_frame_plain() - before transmitting, otherwise the
 *       frame carries no MAC and no CRC.
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
   * @brief Create a stop frame
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @return true on success
   */
  bool create_stop_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE]
  );

  /**
   * @brief Create emergency close frame (for rain)
   *
   * Uses the environment-protection priority level in the ACEI byte and the
   * rain-sensor originator, so it outranks ordinary user commands.
   *
   * @param frame Output IoFrame (not finalized - see class note)
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
 *
 * @note As with VeluxWindow, the create_*_frame() helpers do not finalize the
 *       frame; call frame::finalize_frame() before transmitting.
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
   * @brief Create a position frame
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @param percent_open 0 = closed, 100 = open
   * @return true on success
   */
  bool create_position_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE],
    uint8_t percent_open
  );

  /**
   * @brief Create a tilt frame
   *
   * The tilt value travels in Functional Parameter 1 while the Main Parameter
   * holds "current position" (0xD200), so the blind changes slat angle without
   * moving. FP1 is one byte on the 0..0xC8 scale, i.e. the high byte of the
   * 16-bit 0..0xC800 percentage range.
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @param percent_open Slat opening, 0 = fully closed, 100 = fully open.
   *        Values above 100 are clamped.
   *
   *        This used to be `tilt_percent`, with no direction given. The
   *        implementation treated it as a *closure* percentage, because that is
   *        what the underlying FP1 scale counts - so a caller reading the name
   *        as "how far open" got the opposite of what it asked for, which is
   *        what happened in the ESPHome cover. It now takes an opening
   *        percentage and inverts internally, matching
   *        create_position_frame(percent_open).
   *
   * @return true on success, false if the model has no tilt
   *
   * @warning The direction of FP1 is inferred from it sharing the Main
   *          Parameter's scale, not from a capture. If a blind tilts the wrong
   *          way, this is the first thing to suspect.
   */
  bool create_tilt_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE],
    uint8_t percent_open
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
