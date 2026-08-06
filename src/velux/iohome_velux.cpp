/**
 * @file iohome_velux.cpp
 * @brief Velux-specific features implementation
 * @author iown-homecontrol project
 *
 * Position and ventilation commands are emitted as standard "Activate/Execute
 * Function" frames (command 0x00) carrying a Main Parameter. That is how
 * io-homecontrol actuators are driven; there is no separate "set position"
 * command ID.
 */

#include "iohome_velux.h"
#include <string.h>

namespace iohome {
namespace velux {
namespace {

/**
 * @brief Fill in the common header of a Velux control frame.
 *
 * Frames are left un-finalized: the caller owns the key (1W) or the challenge
 * (2W) and must call frame::finalize_frame() before transmitting.
 */
void begin_control_frame(frame::IoFrame* frame,
                         const uint8_t dest_node[NODE_ID_SIZE],
                         const uint8_t src_node[NODE_ID_SIZE]) {
  frame::init_frame(frame, true);  // 1W - most Velux actuators are 1W driven
  frame::set_destination(frame, dest_node);
  frame::set_source(frame, src_node);
}

/// Convert a "percent open" value into the Main Parameter, which counts closure.
uint16_t main_param_for_percent_open(uint8_t percent_open) {
  if (percent_open > 100) {
    percent_open = 100;
  }
  return mp_from_percent_closed(static_cast<uint8_t>(100 - percent_open));
}

} // namespace

// ============================================================================
// VeluxWindow Implementation
// ============================================================================

VeluxWindow::VeluxWindow(const uint8_t node_id[NODE_ID_SIZE], VeluxModel model)
  : model_(model),
    rain_protection_enabled_(false),
    last_rain_status_(RainSensorStatus::UNKNOWN)
{
  if (node_id != nullptr) {
    memcpy(node_id_, node_id, NODE_ID_SIZE);
  } else {
    memset(node_id_, 0, NODE_ID_SIZE);
  }
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
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  if (level > 3) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);

  const uint8_t percent_open = get_ventilation_position(level);
  return frame::set_execute_command(frame, main_param_for_percent_open(percent_open));
}

bool VeluxWindow::create_position_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE],
  WindowPosition position
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);

  const uint8_t percent_open = static_cast<uint8_t>(position);
  return frame::set_execute_command(frame, main_param_for_percent_open(percent_open));
}

bool VeluxWindow::create_stop_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE]
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);
  return frame::set_execute_command(frame, MP_STOP);
}

bool VeluxWindow::create_rain_close_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE]
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);

  // Priority lives in the ACEI byte, not in Control Byte 1. Environment
  // protection outranks ordinary user commands, which is what a rain-triggered
  // close needs. Bit 0 (IsValid) stays set.
  const uint8_t acei = make_acei(PriorityLevel::ENVIRONMENT_PROTECTION);
  return frame::set_execute_command(frame, MP_CLOSE, Originator::SENSOR_RAIN, acei);
}

bool VeluxWindow::create_emergency_close_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE]
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);

  // Emergency is the originator, not a command of its own - and not priority
  // level 0, which disables every other category and is not ours to use.
  const uint8_t acei = make_acei(PriorityLevel::ENVIRONMENT_PROTECTION);
  return frame::set_execute_command(frame, MP_CLOSE, Originator::EMERGENCY, acei);
}

bool VeluxWindow::create_secured_ventilation_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE]
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);
  return frame::set_execute_command(frame, MP_SECURED_VENTILATION);
}

RainSensorStatus VeluxWindow::parse_rain_sensor_status(const frame::IoFrame* frame) {
  if (frame == nullptr) {
    return RainSensorStatus::UNKNOWN;
  }

  // Rain shows up as an ordinary command that a rain sensor originated, not as
  // a reply to a query. The originator is the first byte of the Execute
  // payload, so a frame without one says nothing either way.
  if (frame->command_id != CMD_EXECUTE || frame->data_len < 1) {
    return RainSensorStatus::UNKNOWN;
  }

  return (frame->data[EXECUTE_OFFSET_ORIGINATOR] ==
          static_cast<uint8_t>(Originator::SENSOR_RAIN))
           ? RainSensorStatus::RAIN
           : RainSensorStatus::UNKNOWN;
}

// ============================================================================
// VeluxBlind Implementation
// ============================================================================

VeluxBlind::VeluxBlind(const uint8_t node_id[NODE_ID_SIZE], VeluxModel model)
  : model_(model)
{
  if (node_id != nullptr) {
    memcpy(node_id_, node_id, NODE_ID_SIZE);
  } else {
    memset(node_id_, 0, NODE_ID_SIZE);
  }
}

size_t VeluxBlind::get_recommended_positions(uint8_t positions[5]) const {
  if (positions == nullptr) {
    return 0;
  }

  switch (model_) {
    case VeluxModel::DML:  // Blackout blind
      positions[0] = 0;
      positions[1] = 50;
      positions[2] = 100;
      return 3;

    case VeluxModel::RML:  // Roller blind
      positions[0] = 0;
      positions[1] = 25;
      positions[2] = 50;
      positions[3] = 75;
      positions[4] = 100;
      return 5;

    case VeluxModel::MML:  // Awning blind (outside)
    case VeluxModel::SML:  // Roller shutter (outside)
      // Fewer positions for weather protection
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
      positions[0] = 0;
      positions[1] = 50;
      positions[2] = 100;
      return 3;
  }
}

bool VeluxBlind::supports_tilt() const {
  // Pleated blinds are the only Velux blind in this list with a slat angle to
  // control; the roller and blackout types have none.
  return model_ == VeluxModel::FML;
}

bool VeluxBlind::create_position_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE],
  uint8_t percent_open
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  begin_control_frame(frame, node_id_, src_node);
  return frame::set_execute_command(frame, main_param_for_percent_open(percent_open));
}

bool VeluxBlind::create_tilt_frame(
  frame::IoFrame* frame,
  const uint8_t src_node[NODE_ID_SIZE],
  uint8_t percent_open
) {
  if (frame == nullptr || src_node == nullptr) {
    return false;
  }

  if (!supports_tilt()) {
    return false;
  }

  if (percent_open > 100) {
    percent_open = 100;
  }

  begin_control_frame(frame, node_id_, src_node);

  // Tilt is carried in Functional Parameter 1 while the Main Parameter keeps
  // the current position (0xD200). FP1 uses the same scale as the main
  // parameter, in a single byte: it counts closure, so 0x00 is open and 0xC8
  // is closed. The caller passes an opening percentage, like
  // create_position_frame() does, and the inversion happens here.
  const uint8_t percent_closed = static_cast<uint8_t>(100u - percent_open);
  const uint8_t fp1 = static_cast<uint8_t>((static_cast<uint16_t>(percent_closed) * 200u) / 100u);
  return frame::set_execute_command(frame, MP_STOP, Originator::USER, ACEI_DEFAULT, fp1);
}

// ============================================================================
// Helper Functions
// ============================================================================

VeluxCategory detect_category(uint16_t node_type, uint8_t manufacturer) {
  if (manufacturer != static_cast<uint8_t>(Manufacturer::VELUX)) {
    return VeluxCategory::UNKNOWN;
  }

  // Compare on the 10-bit type, so every sub-type of a kind lands in the same
  // category: a window opener with an integrated rain sensor (0x0101) is still
  // a window.
  switch (node_type_of(node_type)) {
    case node_type_of(static_cast<uint16_t>(NodeType::WINDOW_OPENER)):
      return VeluxCategory::WINDOW;
    case node_type_of(static_cast<uint16_t>(NodeType::ROLLER_SHUTTER)):
    case node_type_of(static_cast<uint16_t>(NodeType::DUAL_ROLLER_SHUTTER)):
      return VeluxCategory::ROLLER_SHUTTER;
    case node_type_of(static_cast<uint16_t>(NodeType::INTERIOR_VENETIAN_BLIND)):
    case node_type_of(static_cast<uint16_t>(NodeType::VERTICAL_INTERIOR_BLIND)):
    case node_type_of(static_cast<uint16_t>(NodeType::EXTERIOR_VENETIAN_BLIND)):
    case node_type_of(static_cast<uint16_t>(NodeType::LOUVER_BLIND)):
      return VeluxCategory::BLIND;
    case node_type_of(static_cast<uint16_t>(NodeType::VERTICAL_EXTERIOR_AWNING)):
    case node_type_of(static_cast<uint16_t>(NodeType::HORIZONTAL_AWNING)):
      return VeluxCategory::AWNING;
    case node_type_of(static_cast<uint16_t>(NodeType::LIGHT)):
      return VeluxCategory::LIGHT;
    case node_type_of(static_cast<uint16_t>(NodeType::REMOTE_CONTROLLER)):
    case node_type_of(static_cast<uint16_t>(NodeType::BEACON)):
      return VeluxCategory::CONTROLLER;
    default:
      return VeluxCategory::UNKNOWN;
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

    case VeluxModel::UNKNOWN:
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
  // Rain sensors ship with the powered roof windows.
  return model == VeluxModel::GGL_ELECTRIC ||
         model == VeluxModel::GGU_ELECTRIC ||
         model == VeluxModel::GGL_SOLAR ||
         model == VeluxModel::GGU_SOLAR;
}

uint8_t get_recommended_ventilation(float indoor_temp_celsius) {
  if (indoor_temp_celsius < 18.0f) {
    return 0;  // Too cold, keep closed
  }
  if (indoor_temp_celsius < 22.0f) {
    return 1;  // Comfortable, minimal ventilation
  }
  if (indoor_temp_celsius < 25.0f) {
    return 2;  // Warm, medium ventilation
  }
  return 3;    // Hot, maximum ventilation
}

} // namespace velux
} // namespace iohome
