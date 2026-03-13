/**
 * @file iohome_velux.cpp
 * @brief Velux-specific features implementation
 * @author iown-homecontrol project
 */

#include "iohome_velux.h"
#include <string.h>

namespace iohome {
namespace velux {

// ============================================================================
// VeluxWindow Implementation
// ============================================================================

VeluxWindow::VeluxWindow(const uint8_t node_id[NODE_ID_SIZE], VeluxModel model)
  : model_(model),
    rain_protection_enabled_(false),
    last_rain_status_(RainSensorStatus::UNKNOWN)
{
  memcpy(node_id_, node_id, NODE_ID_SIZE);
}

uint8_t VeluxWindow::get_ventilation_position(uint8_t level) const {
  switch (level) {
    case 1:
      return static_cast<uint8_t>(WindowPosition::VENTILATION_1);
    case 2:
      return static_cast<uint8_t>(WindowPosition::VENTILATION_2);
    case 3:
      return static_cast<uint8_t>(WindowPosition::VENTILATION_3);
    default:
      return static_cast<uint8_t>(WindowPosition::CLOSED);
  }
}

bool VeluxWindow::create_ventilation_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE],
  uint8_t level
) {
  // Initialize frame
  frame::init_frame(frame, true);  // 1W mode (most Velux windows)
  frame::set_destination(frame, node_id_);
  frame::set_source(frame, src_node);

  // Get ventilation position
  uint8_t position = get_ventilation_position(level);

  // Use standard position command
  uint8_t params[2] = {position, 0x00};
  return frame::set_command(frame, CMD_SET_POSITION, params, 2);
}

bool VeluxWindow::create_position_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE],
  WindowPosition position
) {
  frame::init_frame(frame, true);
  frame::set_destination(frame, node_id_);
  frame::set_source(frame, src_node);

  uint8_t pos_value = static_cast<uint8_t>(position);
  uint8_t params[2] = {pos_value, 0x00};
  return frame::set_command(frame, CMD_SET_POSITION, params, 2);
}

bool VeluxWindow::create_emergency_close_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE]
) {
  frame::init_frame(frame, true);
  frame::set_destination(frame, node_id_);
  frame::set_source(frame, src_node);

  // Emergency close has priority flag
  frame->ctrl_byte_1 |= 0x10;  // Set priority bit

  // Close position (0%)
  uint8_t params[2] = {0x00, 0x00};
  return frame::set_command(frame, VELUX_CMD_EMERGENCY_CLOSE, params, 2);
}

RainSensorStatus VeluxWindow::parse_rain_sensor_status(const frame::IoFrame* frame) {
  if (frame->command_id != VELUX_CMD_GET_RAIN_SENSOR) {
    return RainSensorStatus::UNKNOWN;
  }

  if (frame->data_len < 1) {
    return RainSensorStatus::UNKNOWN;
  }

  switch (frame->data[0]) {
    case 0x01:
      return RainSensorStatus::DRY;
    case 0x02:
      return RainSensorStatus::RAIN;
    case 0xFF:
      return RainSensorStatus::ERROR;
    default:
      return RainSensorStatus::UNKNOWN;
  }
}

// ============================================================================
// VeluxBlind Implementation
// ============================================================================

VeluxBlind::VeluxBlind(const uint8_t node_id[NODE_ID_SIZE], VeluxModel model)
  : model_(model)
{
  memcpy(node_id_, node_id, NODE_ID_SIZE);
}

size_t VeluxBlind::get_recommended_positions(uint8_t positions[5]) const {
  switch (model_) {
    case VeluxModel::DML:  // Blackout blind
      // Fully closed, half, fully open
      positions[0] = 0;
      positions[1] = 50;
      positions[2] = 100;
      return 3;

    case VeluxModel::RML:  // Roller blind
      // Closed, quarter, half, three-quarter, open
      positions[0] = 0;
      positions[1] = 25;
      positions[2] = 50;
      positions[3] = 75;
      positions[4] = 100;
      return 5;

    case VeluxModel::MML:  // Awning blind (outside)
    case VeluxModel::SML:  // Roller shutter (outside)
      // Closed, half, open (less positions for weather protection)
      positions[0] = 0;
      positions[1] = 50;
      positions[2] = 100;
      return 3;

    case VeluxModel::FML:  // Pleated blind
      // More positions for light control
      positions[0] = 0;
      positions[1] = 20;
      positions[2] = 40;
      positions[3] = 60;
      positions[4] = 100;
      return 5;

    default:
      // Generic: closed, half, open
      positions[0] = 0;
      positions[1] = 50;
      positions[2] = 100;
      return 3;
  }
}

bool VeluxBlind::supports_tilt() const {
  // Only venetian blinds support tilt
  return model_ == VeluxModel::FML;  // Pleated blind has limited tilt
}

bool VeluxBlind::create_tilt_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE],
  uint8_t tilt_angle
) {
  if (!supports_tilt()) {
    return false;
  }

  frame::init_frame(frame, true);
  frame::set_destination(frame, node_id_);
  frame::set_source(frame, src_node);

  // Tilt command (custom Velux command)
  uint8_t params[2] = {tilt_angle, 0x00};
  return frame::set_command(frame, 0x65, params, 2);  // Tilt command
}

// ============================================================================
// Helper Functions
// ============================================================================

VeluxModel detect_model(uint8_t device_type, uint8_t manufacturer) {
  // Manufacturer code 0x01 = Velux
  if (manufacturer != 0x01) {
    return VeluxModel::UNKNOWN;
  }

  // Map device type to Velux model
  switch (device_type) {
    case 0x03:  // Window opener
      return VeluxModel::GGL_ELECTRIC;

    case 0x00:  // Roller shutter
      return VeluxModel::SML;

    case 0x04:  // Venetian blind
      return VeluxModel::FML;

    case 0x05:  // Exterior blind
      return VeluxModel::MML;

    default:
      return VeluxModel::UNKNOWN;
  }
}

const char* get_model_name(VeluxModel model) {
  switch (model) {
    case VeluxModel::GGL:
      return "GGL - Top-operated roof window";
    case VeluxModel::GGU:
      return "GGU - Top-operated roof window";
    case VeluxModel::GPL:
      return "GPL - Top-operated roof window";
    case VeluxModel::GPU:
      return "GPU - Top-operated roof window";

    case VeluxModel::GGL_SOLAR:
      return "GGL Solar - Solar powered window";
    case VeluxModel::GGU_SOLAR:
      return "GGU Solar - Solar powered window";

    case VeluxModel::GGL_ELECTRIC:
      return "GGL Electric (KMX 100)";
    case VeluxModel::GGU_ELECTRIC:
      return "GGU Electric (KMX 200)";

    case VeluxModel::DML:
      return "DML - Blackout blind";
    case VeluxModel::RML:
      return "RML - Roller blind";
    case VeluxModel::FML:
      return "FML - Pleated blind";
    case VeluxModel::MML:
      return "MML - Awning blind";
    case VeluxModel::SML:
      return "SML - Roller shutter";

    case VeluxModel::KLR_200:
      return "KLR 200 - Remote control";
    case VeluxModel::KLI_310:
      return "KLI 310 - Wall switch";
    case VeluxModel::KLF_200:
      return "KLF 200 - Internet gateway";

    default:
      return "Unknown Velux device";
  }
}

bool is_roof_window(VeluxModel model) {
  return (model >= VeluxModel::GGL && model <= VeluxModel::GPU) ||
         (model >= VeluxModel::GGL_SOLAR && model <= VeluxModel::GGU_SOLAR) ||
         (model >= VeluxModel::GGL_ELECTRIC && model <= VeluxModel::GGU_ELECTRIC);
}

bool is_blind(VeluxModel model) {
  return model >= VeluxModel::DML && model <= VeluxModel::SML;
}

bool supports_rain_sensor(VeluxModel model) {
  // Most electric roof windows support rain sensor
  return is_roof_window(model) &&
         (model == VeluxModel::GGL_ELECTRIC ||
          model == VeluxModel::GGU_ELECTRIC ||
          model == VeluxModel::GGL_SOLAR ||
          model == VeluxModel::GGU_SOLAR);
}

uint8_t get_recommended_ventilation(float indoor_temp_celsius) {
  // Intelligent ventilation based on temperature
  if (indoor_temp_celsius < 18.0f) {
    return 0;  // Too cold, keep closed
  } else if (indoor_temp_celsius < 22.0f) {
    return 1;  // Comfortable, minimal ventilation
  } else if (indoor_temp_celsius < 25.0f) {
    return 2;  // Warm, medium ventilation
  } else {
    return 3;  // Hot, maximum ventilation
  }
}

} // namespace velux
} // namespace iohome
