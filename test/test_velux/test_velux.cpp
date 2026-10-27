/**
 * Unit tests for the Velux helpers.
 *
 * The helpers build ordinary "Activate/Execute Function" frames (command 0x00);
 * these tests pin down the Main Parameter each one produces, since that is what
 * actually decides whether a window opens or closes.
 */

#include <unity.h>

#include "velux/iohome_velux.h"

#include <string.h>

namespace velux = iohome::velux;

namespace {

const uint8_t WINDOW_NODE[3] = {0xAA, 0xBB, 0xCC};
const uint8_t SRC_NODE[3] = {0x1A, 0x38, 0x0B};

uint16_t main_param_of(const iohome::frame::IoFrame& frame) {
  return static_cast<uint16_t>((static_cast<uint16_t>(frame.data[2]) << 8) | frame.data[3]);
}

}  // namespace

// ---------------------------------------------------------------------------
// VeluxWindow
// ---------------------------------------------------------------------------

void test_window_uses_execute_command(void) {
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_ELECTRIC);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_position_frame(&frame, SRC_NODE,
                                                velux::WindowPosition::FULLY_OPEN));

  // Regression: these helpers used to emit command 0x60, which does not exist.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, frame.command_id);
  TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_MIN_SIZE, frame.data_len);
  TEST_ASSERT_TRUE(iohome::is_acei_valid(frame.data[1]));

  TEST_ASSERT_EQUAL_UINT8_ARRAY(WINDOW_NODE, frame.dest_node, 3);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(SRC_NODE, frame.src_node, 3);
}

void test_window_position_mapping(void) {
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_ELECTRIC);
  iohome::frame::IoFrame frame;

  // Fully open is the minimum wire value, fully closed the maximum.
  TEST_ASSERT_TRUE(
      window.create_position_frame(&frame, SRC_NODE, velux::WindowPosition::FULLY_OPEN));
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_OPEN, main_param_of(frame));

  TEST_ASSERT_TRUE(window.create_position_frame(&frame, SRC_NODE, velux::WindowPosition::CLOSED));
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_CLOSE, main_param_of(frame));

  TEST_ASSERT_TRUE(
      window.create_position_frame(&frame, SRC_NODE, velux::WindowPosition::HALF_OPEN));
  TEST_ASSERT_EQUAL_HEX16(0x6400, main_param_of(frame));
}

void test_ventilation_levels(void) {
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_ELECTRIC);

  TEST_ASSERT_EQUAL_UINT8(0, window.get_ventilation_position(0));
  TEST_ASSERT_EQUAL_UINT8(10, window.get_ventilation_position(1));
  TEST_ASSERT_EQUAL_UINT8(20, window.get_ventilation_position(2));
  TEST_ASSERT_EQUAL_UINT8(30, window.get_ventilation_position(3));
  TEST_ASSERT_EQUAL_UINT8(0, window.get_ventilation_position(9));

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_ventilation_frame(&frame, SRC_NODE, 1));

  // 10 % open means 90 % closed on the wire.
  TEST_ASSERT_EQUAL_HEX16(iohome::mp_from_percent_closed(90), main_param_of(frame));

  TEST_ASSERT_FALSE(window.create_ventilation_frame(&frame, SRC_NODE, 4));
}

void test_stop_frame(void) {
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_ELECTRIC);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_stop_frame(&frame, SRC_NODE));
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_STOP, main_param_of(frame));
}

void test_protective_closes_use_the_right_originator(void) {
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_ELECTRIC);
  iohome::frame::IoFrame frame;

  // A rain close names the rain sensor. The specification uses exactly this
  // case to describe priority level 1: "rain sensor on a roof window".
  TEST_ASSERT_TRUE(window.create_rain_close_frame(&frame, SRC_NODE));
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_CLOSE, main_param_of(frame));
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(iohome::Originator::SENSOR_RAIN),
                         frame.data[iohome::EXECUTE_OFFSET_ORIGINATOR]);

  // An emergency close names EMERGENCY. The function that builds it used to
  // be the rain one under another name, so both said SENSOR_RAIN.
  TEST_ASSERT_TRUE(window.create_emergency_close_frame(&frame, SRC_NODE));
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_CLOSE, main_param_of(frame));
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(iohome::Originator::EMERGENCY),
                         frame.data[iohome::EXECUTE_OFFSET_ORIGINATOR]);

  // Priority lives in the ACEI byte, not in Control Byte 1. Regression: this
  // used to set bit 4 of Control Byte 1, which is the ACK flag.
  //
  // Environment Protection, not Human Protection: level 0 disables every other
  // category and the specification conditions its use on an agreement from
  // io-homecontrol.
  const uint8_t acei = frame.data[iohome::EXECUTE_OFFSET_ACEI];
  TEST_ASSERT_TRUE(iohome::is_acei_valid(acei));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(iohome::PriorityLevel::ENVIRONMENT_PROTECTION),
      (acei & iohome::ACEI_LEVEL_MASK) >> iohome::ACEI_LEVEL_SHIFT);
  TEST_ASSERT_EQUAL_UINT8(0, frame.ctrl_byte_1 & iohome::CTRL1_ACK);
}

void test_window_helpers_reject_nullptr(void) {
  velux::VeluxWindow window(nullptr, velux::VeluxModel::UNKNOWN);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_FALSE(window.create_position_frame(nullptr, SRC_NODE,
                                                 velux::WindowPosition::CLOSED));
  TEST_ASSERT_FALSE(window.create_position_frame(&frame, nullptr,
                                                 velux::WindowPosition::CLOSED));
  TEST_ASSERT_FALSE(window.create_ventilation_frame(&frame, nullptr, 1));
  TEST_ASSERT_FALSE(window.create_stop_frame(&frame, nullptr));
  TEST_ASSERT_FALSE(window.create_emergency_close_frame(&frame, nullptr));

  // A null node ID must leave the address zeroed, not read out of bounds.
  const uint8_t zero[3] = {0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, window.get_node_id(), 3);
}

void test_rain_sensor_parsing(void) {
  // Rain is not a query and not an answer: it is an ordinary Execute that a
  // rain sensor originated. This used to look for command 0x58 with a
  // DRY/RAIN/ERROR byte, which no device sends and which claimed to observe a
  // "dry" state that nothing ever reports.
  const uint8_t node[3] = {0x0A, 0x0B, 0x0C};
  const uint8_t src[3] = {0x01, 0x02, 0x03};
  velux::VeluxWindow window(node);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_rain_close_frame(&frame, src));
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::RAIN,
                    velux::VeluxWindow::parse_rain_sensor_status(&frame));

  // The same command from a user is not a rain report.
  TEST_ASSERT_TRUE(window.create_emergency_close_frame(&frame, src));
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::UNKNOWN,
                    velux::VeluxWindow::parse_rain_sensor_status(&frame));

  // Neither is a frame that is not an Execute at all, nor an empty one.
  iohome::frame::set_command(&frame, iohome::CMD_GET_NAME, nullptr, 0);
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::UNKNOWN,
                    velux::VeluxWindow::parse_rain_sensor_status(&frame));

  TEST_ASSERT_EQUAL(velux::RainSensorStatus::UNKNOWN,
                    velux::VeluxWindow::parse_rain_sensor_status(nullptr));
}

void test_secured_ventilation_frame(void) {
  // The ventilation position a Velux window actually has: Main Parameter
  // 0xD803 on the ordinary Execute, from the window opener actuator profile.
  const uint8_t node[3] = {0x0A, 0x0B, 0x0C};
  const uint8_t src[3] = {0x01, 0x02, 0x03};
  velux::VeluxWindow window(node);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_secured_ventilation_frame(&frame, src));

  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, frame.command_id);
  TEST_ASSERT_EQUAL_HEX8(0xD8, frame.data[iohome::EXECUTE_OFFSET_MAIN_PARAM]);
  TEST_ASSERT_EQUAL_HEX8(0x03, frame.data[iohome::EXECUTE_OFFSET_MAIN_PARAM + 1]);

  TEST_ASSERT_FALSE(window.create_secured_ventilation_frame(nullptr, src));
  TEST_ASSERT_FALSE(window.create_secured_ventilation_frame(&frame, nullptr));
}

void test_force_frame(void) {
  // The observed "force" preset: Main Parameter 0x6400 on the ordinary Execute.
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_SOLAR);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_force_frame(&frame, SRC_NODE));

  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, frame.command_id);
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_FORCE, main_param_of(frame));

  TEST_ASSERT_FALSE(window.create_force_frame(nullptr, SRC_NODE));
  TEST_ASSERT_FALSE(window.create_force_frame(&frame, nullptr));
}

// ---------------------------------------------------------------------------
// VeluxBlind
// ---------------------------------------------------------------------------

void test_blind_position_frame(void) {
  velux::VeluxBlind blind(WINDOW_NODE, velux::VeluxModel::RML);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(blind.create_position_frame(&frame, SRC_NODE, 25));

  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, frame.command_id);
  TEST_ASSERT_EQUAL_HEX16(iohome::mp_from_percent_closed(75), main_param_of(frame));
}

void test_blind_position_clamps(void) {
  velux::VeluxBlind blind(WINDOW_NODE, velux::VeluxModel::RML);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(blind.create_position_frame(&frame, SRC_NODE, 200));
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_OPEN, main_param_of(frame));
}

void test_blind_stop_frame(void) {
  // A roller shutter (SML) travels, so it needs a stop just like a window:
  // Main Parameter 0xD200 (Current) on the ordinary Execute.
  velux::VeluxBlind roller(WINDOW_NODE, velux::VeluxModel::SML);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(roller.create_stop_frame(&frame, SRC_NODE));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, frame.command_id);
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_STOP, main_param_of(frame));

  TEST_ASSERT_FALSE(roller.create_stop_frame(nullptr, SRC_NODE));
  TEST_ASSERT_FALSE(roller.create_stop_frame(&frame, nullptr));
}

void test_tilt_only_for_models_that_have_it(void) {
  velux::VeluxBlind roller(WINDOW_NODE, velux::VeluxModel::RML);
  velux::VeluxBlind pleated(WINDOW_NODE, velux::VeluxModel::FML);

  TEST_ASSERT_FALSE(roller.supports_tilt());
  TEST_ASSERT_TRUE(pleated.supports_tilt());

  iohome::frame::IoFrame frame;
  TEST_ASSERT_FALSE(roller.create_tilt_frame(&frame, SRC_NODE, 50));
  TEST_ASSERT_TRUE(pleated.create_tilt_frame(&frame, SRC_NODE, 50));

  // Tilt rides in FP1 while the main parameter holds "current position", so
  // the slats turn without the blind travelling.
  TEST_ASSERT_EQUAL_HEX16(iohome::MP_STOP, main_param_of(frame));
  TEST_ASSERT_EQUAL_UINT8(100, frame.data[4]);  // 50 % either way

  // FP1 counts closure while the argument is an opening percentage, the same
  // way create_position_frame() works. Fully open must be the low end of the
  // scale; sending it straight through would close the slats instead.
  TEST_ASSERT_TRUE(pleated.create_tilt_frame(&frame, SRC_NODE, 100));
  TEST_ASSERT_EQUAL_UINT8(0, frame.data[4]);

  TEST_ASSERT_TRUE(pleated.create_tilt_frame(&frame, SRC_NODE, 0));
  TEST_ASSERT_EQUAL_UINT8(200, frame.data[4]);

  // 25 % open is 75 % closed, which is 150 on the 0..200 scale.
  TEST_ASSERT_TRUE(pleated.create_tilt_frame(&frame, SRC_NODE, 25));
  TEST_ASSERT_EQUAL_UINT8(150, frame.data[4]);

  TEST_ASSERT_TRUE(pleated.create_tilt_frame(&frame, SRC_NODE, 250));
  TEST_ASSERT_EQUAL_UINT8(0, frame.data[4]);  // clamped to fully open
}

void test_recommended_positions(void) {
  uint8_t positions[5] = {0};

  velux::VeluxBlind roller(WINDOW_NODE, velux::VeluxModel::RML);
  TEST_ASSERT_EQUAL_UINT(5, roller.get_recommended_positions(positions));
  TEST_ASSERT_EQUAL_UINT8(0, positions[0]);
  TEST_ASSERT_EQUAL_UINT8(100, positions[4]);

  velux::VeluxBlind blackout(WINDOW_NODE, velux::VeluxModel::DML);
  TEST_ASSERT_EQUAL_UINT(3, blackout.get_recommended_positions(positions));

  TEST_ASSERT_EQUAL_UINT(0, roller.get_recommended_positions(nullptr));
}

// ---------------------------------------------------------------------------
// Model helpers
// ---------------------------------------------------------------------------

void test_model_detection(void) {
  // A node announces its type, not its product number. detect_model() used to
  // map "window opener" to GGL_ELECTRIC and "venetian blind" to FML - specific
  // Velux products picked from a field that cannot tell them apart. What the
  // type field does determine is the category.
  using velux::VeluxCategory;
  const uint8_t VLX = static_cast<uint8_t>(iohome::Manufacturer::VELUX);

  TEST_ASSERT_EQUAL(VeluxCategory::WINDOW,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::WINDOW_OPENER), VLX));

  // Every sub-type of a kind lands in the same category: a window opener with
  // an integrated rain sensor is still a window.
  TEST_ASSERT_EQUAL(VeluxCategory::WINDOW,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::WINDOW_OPENER_RAIN_SENSOR), VLX));

  TEST_ASSERT_EQUAL(VeluxCategory::ROLLER_SHUTTER,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::ROLLER_SHUTTER), VLX));
  TEST_ASSERT_EQUAL(VeluxCategory::ROLLER_SHUTTER,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::ROLLER_SHUTTER_ADJUSTABLE), VLX));
  TEST_ASSERT_EQUAL(VeluxCategory::BLIND,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::INTERIOR_VENETIAN_BLIND), VLX));
  TEST_ASSERT_EQUAL(VeluxCategory::AWNING,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::HORIZONTAL_AWNING), VLX));
  TEST_ASSERT_EQUAL(VeluxCategory::CONTROLLER,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::REMOTE_CONTROLLER), VLX));

  // Another manufacturer is never a Velux product.
  TEST_ASSERT_EQUAL(VeluxCategory::UNKNOWN,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::WINDOW_OPENER),
                        static_cast<uint8_t>(iohome::Manufacturer::SOMFY)));
  TEST_ASSERT_EQUAL(VeluxCategory::UNKNOWN,
                    velux::detect_category(
                        static_cast<uint16_t>(iohome::NodeType::HEAT_PUMP), VLX));
}

void test_model_classification(void) {
  TEST_ASSERT_TRUE(velux::is_roof_window(velux::VeluxModel::GGL));
  TEST_ASSERT_TRUE(velux::is_roof_window(velux::VeluxModel::GGL_ELECTRIC));
  TEST_ASSERT_FALSE(velux::is_roof_window(velux::VeluxModel::RML));
  TEST_ASSERT_FALSE(velux::is_roof_window(velux::VeluxModel::UNKNOWN));

  TEST_ASSERT_TRUE(velux::is_blind(velux::VeluxModel::DML));
  TEST_ASSERT_TRUE(velux::is_blind(velux::VeluxModel::SML));
  TEST_ASSERT_FALSE(velux::is_blind(velux::VeluxModel::GGL));
  TEST_ASSERT_FALSE(velux::is_blind(velux::VeluxModel::KLR_200));

  TEST_ASSERT_TRUE(velux::supports_rain_sensor(velux::VeluxModel::GGL_ELECTRIC));
  TEST_ASSERT_TRUE(velux::supports_rain_sensor(velux::VeluxModel::GGU_SOLAR));
  TEST_ASSERT_FALSE(velux::supports_rain_sensor(velux::VeluxModel::GGL));
  TEST_ASSERT_FALSE(velux::supports_rain_sensor(velux::VeluxModel::RML));

  TEST_ASSERT_NOT_NULL(velux::get_model_name(velux::VeluxModel::UNKNOWN));
  TEST_ASSERT_NOT_NULL(velux::get_model_name(static_cast<velux::VeluxModel>(0x7F)));
}

void test_recommended_ventilation(void) {
  TEST_ASSERT_EQUAL_UINT8(0, velux::get_recommended_ventilation(10.0f));
  TEST_ASSERT_EQUAL_UINT8(1, velux::get_recommended_ventilation(20.0f));
  TEST_ASSERT_EQUAL_UINT8(2, velux::get_recommended_ventilation(23.0f));
  TEST_ASSERT_EQUAL_UINT8(3, velux::get_recommended_ventilation(30.0f));
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_window_uses_execute_command);
  RUN_TEST(test_window_position_mapping);
  RUN_TEST(test_ventilation_levels);
  RUN_TEST(test_stop_frame);
  RUN_TEST(test_protective_closes_use_the_right_originator);
  RUN_TEST(test_window_helpers_reject_nullptr);
  RUN_TEST(test_rain_sensor_parsing);
  RUN_TEST(test_secured_ventilation_frame);
  RUN_TEST(test_force_frame);

  RUN_TEST(test_blind_position_frame);
  RUN_TEST(test_blind_position_clamps);
  RUN_TEST(test_blind_stop_frame);
  RUN_TEST(test_tilt_only_for_models_that_have_it);
  RUN_TEST(test_recommended_positions);

  RUN_TEST(test_model_detection);
  RUN_TEST(test_model_classification);
  RUN_TEST(test_recommended_ventilation);

  return UNITY_END();
}
