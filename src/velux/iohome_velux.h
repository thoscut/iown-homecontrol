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
 * @brief What a received frame says about rain
 *
 * A rain sensor is an input, not something a controller polls. It cannot be
 * asked whether it is dry - there is no query, and DRY and ERROR were never
 * observable. What *is* observable is the sensor acting: the frame it causes
 * carries Command Originator 0x02 (RAIN). So this has two states, and the
 * absence of rain is simply the absence of such a frame.
 */
enum class RainSensorStatus : uint8_t {
  /// Nothing in this frame says anything about rain.
  UNKNOWN = 0x00,
  /// This frame is an actuator command a rain sensor triggered.
  RAIN = 0x02
};

// ============================================================================
// There are no Velux-private command IDs here, and there never were
//
// This file used to declare six of them - 0x58 GET_RAIN_SENSOR, 0x59
// SET_VENTILATION, 0x5A EMERGENCY_CLOSE, 0x5B GET_WINDOW_STATUS, 0x5C
// RESET_LIMITS, 0x5D SET_LIMITS - all marked UNVERIFIED. They were invented.
// Two things give them away before any capture is taken:
//
//   * 0x50-0x57 are documented in docs/commands.md as four request/answer
//     *pairs*: Get Name / Get Name Answer, Write Name / Write Name Ack, Get
//     General Info 1 / Answer, Get General Info 2 / Answer. Even is the
//     request, odd is the reply. The six sat in that block as unpaired
//     singletons.
//   * That block is metadata - names and info. Four of the six claimed to be
//     actuator *control*, which lives at 0x00.
//
// Each function they claimed does exist. None of them is a command:
//
//   Rain sensor        A rain sensor is an input, not something to poll. A
//                      window with one is node type 0x0101 (Window Opener with
//                      Integrated Rain Sensor), and when it acts, the frame it
//                      produces carries Command Originator 0x02 (RAIN). The
//                      KLF 200 surfaces the same thing as STATUS_RAIN and
//                      LIMITATION_BY_RAIN. So a controller learns about rain
//                      by watching originators, not by asking.
//
//   Ventilation        Main Parameter 0xD803, "Secured Ventilation", from the
//                      window opener actuator profile. See
//                      MP_SECURED_VENTILATION. An ordinary Execute (0x00).
//
//   Emergency close    Command Originator 0xFF (EMERGENCY) with a priority
//                      level in the Protection group (PL0-PL1), on an ordinary
//                      Execute. Priority is what makes it override; the
//                      command is the same one every other close uses.
//
//   Window status      The actuator reports its own state; a controller reads
//                      the current value with the Current access method
//                      (0xD200) rather than a private query.
//
//   Set/reset limits   Real io-homecontrol functionality - the KLF 200 exposes
//                      it as GW_SET_LIMITATION_REQ and documents the semantics
//                      in §10.5. Its RF command ID is genuinely unknown, and
//                      guessing 0x5C/0x5D did not make it known.
//
// Sources: docs/commands.md, and the Velux KLF 200 API specification in
// docs/devices/velux/KLF200/ - Table 164 (CommandOriginator), Table 165
// (PriorityLevel), Table 275 (Access Methods), §14.2.1 (alias values).
// ============================================================================

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
   * @brief Create a rain-triggered close
   *
   * Command Originator 0x02 (RAIN) at priority level 1, Environment
   * Protection - the level the specification describes with exactly this
   * example, "rain sensor on a roof window". It outranks ordinary user
   * commands, which is the point: the window shuts even if someone just asked
   * for it to be open.
   *
   * This is what create_emergency_close_frame() used to build, under a name
   * that said something else.
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @return true on success
   */
  bool create_rain_close_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE]
  );

  /**
   * @brief Create an emergency close
   *
   * Command Originator 0xFF (EMERGENCY), "used in context with emergency or
   * security commands", again at Environment Protection.
   *
   * Not at level 0, Human Protection, although the name invites it: level 0
   * disables every other category, and the specification makes its use
   * conditional on an agreement from io-homecontrol. A library cannot grant
   * itself that.
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
   * @brief Create a frame for the secured ventilation position
   *
   * The window opens far enough to ventilate while staying locked. This is a
   * position, expressed as Main Parameter 0xD803 on the ordinary Execute
   * command, and it is what a Velux window actually implements - unlike
   * create_ventilation_frame(), which picks a percentage this library chose.
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @return true on success
   */
  bool create_secured_ventilation_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE]
  );

  /**
   * @brief Create a frame for the "force" preset a Velux remote's button sends
   *
   * Main Parameter 0x6400 (MP_FORCE), the value observed from a real remote's
   * dedicated button rather than derived from the specification. On the wire it
   * is identical to a 50 % position; it is kept separate because the button is a
   * fixed preset, not a percentage. See MP_FORCE and docs/VELUX-FORMAT.md.
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @return true on success
   */
  bool create_force_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE]
  );

  /**
   * @brief Whether a received frame is a rain-triggered command
   *
   * There is no rain-sensor query and no rain-sensor answer. What a controller
   * can see is the sensor acting: an Execute whose Command Originator is 0x02
   * (RAIN). This used to look for command 0x58 with a one-byte DRY/RAIN/ERROR
   * payload, which no device sends.
   *
   * @param frame Received frame
   * @return RAIN if this frame was triggered by a rain sensor, else UNKNOWN
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

  /**
   * @brief Create a stop frame
   *
   * A roller shutter or blind is a cover that travels, so it needs a stop just
   * as a window does. Main Parameter 0xD200 (Current) - "hold at the current
   * position". VeluxWindow has had this; VeluxBlind was missing it.
   *
   * @param frame Output IoFrame (not finalized - see class note)
   * @param src_node Source node ID (3 bytes)
   * @return true on success
   */
  bool create_stop_frame(
    frame::IoFrame* frame,
    const uint8_t src_node[NODE_ID_SIZE]
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
 * @brief What kind of Velux product a discovered node is
 *
 * As much as the wire can tell you, and no more. A node announces its *type* -
 * window opener, roller shutter, blind - and that is the whole of it. Which
 * window opener, GGL or GGU or GPL, is a question the type field does not
 * answer: the KLF 200 reads that from a separate ProductType field ("Ex. KMG,
 * KMX etc.", §NodeTypeSubType), and the Discover Answer does not carry one.
 *
 * detect_model() used to claim otherwise. It mapped node type "window opener"
 * to VeluxModel::GGL_ELECTRIC and "venetian blind" to FML - specific product
 * numbers picked from a field that cannot distinguish them. VeluxModel is a
 * configuration value the user supplies, not something to be detected.
 */
enum class VeluxCategory : uint8_t {
  UNKNOWN = 0,
  WINDOW,           // Roof window, with or without an integrated rain sensor
  ROLLER_SHUTTER,   // Outside roller shutter
  BLIND,            // Interior blind - blackout, roller, pleated
  AWNING,           // Awning blind, inside or outside
  LIGHT,
  CONTROLLER        // Remote, wall switch or gateway
};

/**
 * @brief Categorise a discovered node
 *
 * @param node_type Full 16-bit node type field from the discovery answer
 * @param manufacturer OEM ID; anything but Velux gives UNKNOWN
 * @return The category the node type determines
 */
VeluxCategory detect_category(uint16_t node_type, uint8_t manufacturer);

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
