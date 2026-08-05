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

void test_emergency_close_uses_priority(void) {
  velux::VeluxWindow window(WINDOW_NODE, velux::VeluxModel::GGL_ELECTRIC);

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(window.create_emergency_close_frame(&frame, SRC_NODE));

  TEST_ASSERT_EQUAL_HEX16(iohome::MP_CLOSE, main_param_of(frame));
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(iohome::Originator::SENSOR_RAIN), frame.data[0]);

  // Priority lives in the ACEI byte, not in Control Byte 1. Regression: this
  // used to set bit 4 of Control Byte 1, which is the ACK flag.
  const uint8_t acei = frame.data[1];
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
  iohome::frame::IoFrame frame;
  iohome::frame::init_frame(&frame, true);

  const uint8_t dry[] = {0x01};
  iohome::frame::set_command(&frame, velux::VELUX_CMD_GET_RAIN_SENSOR, dry, 1);
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::DRY, velux::VeluxWindow::parse_rain_sensor_status(&frame));

  const uint8_t rain[] = {0x02};
  iohome::frame::set_command(&frame, velux::VELUX_CMD_GET_RAIN_SENSOR, rain, 1);
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::RAIN, velux::VeluxWindow::parse_rain_sensor_status(&frame));

  const uint8_t error[] = {0xFF};
  iohome::frame::set_command(&frame, velux::VELUX_CMD_GET_RAIN_SENSOR, error, 1);
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::ERROR, velux::VeluxWindow::parse_rain_sensor_status(&frame));

  // Wrong command, empty payload and nullptr must all report UNKNOWN.
  iohome::frame::set_command(&frame, iohome::CMD_EXECUTE, dry, 1);
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::UNKNOWN,
                    velux::VeluxWindow::parse_rain_sensor_status(&frame));

  iohome::frame::set_command(&frame, velux::VELUX_CMD_GET_RAIN_SENSOR, nullptr, 0);
  TEST_ASSERT_EQUAL(velux::RainSensorStatus::UNKNOWN,
                    velux::VeluxWindow::parse_rain_sensor_status(&frame));

  TEST_ASSERT_EQUAL(velux::RainSensorStatus::UNKNOWN,
                    velux::VeluxWindow::parse_rain_sensor_status(nullptr));
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
  TEST_ASSERT_EQUAL(velux::VeluxModel::GGL_ELECTRIC, velux::detect_model(0x03, 0x01));
  TEST_ASSERT_EQUAL(velux::VeluxModel::SML, velux::detect_model(0x00, 0x01));
  TEST_ASSERT_EQUAL(velux::VeluxModel::FML, velux::detect_model(0x04, 0x01));
  TEST_ASSERT_EQUAL(velux::VeluxModel::MML, velux::detect_model(0x05, 0x01));

  // Another manufacturer is never a Velux model.
  TEST_ASSERT_EQUAL(velux::VeluxModel::UNKNOWN, velux::detect_model(0x03, 0x02));
  TEST_ASSERT_EQUAL(velux::VeluxModel::UNKNOWN, velux::detect_model(0x7F, 0x01));
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
  RUN_TEST(test_emergency_close_uses_priority);
  RUN_TEST(test_window_helpers_reject_nullptr);
  RUN_TEST(test_rain_sensor_parsing);

  RUN_TEST(test_blind_position_frame);
  RUN_TEST(test_blind_position_clamps);
  RUN_TEST(test_tilt_only_for_models_that_have_it);
  RUN_TEST(test_recommended_positions);

  RUN_TEST(test_model_detection);
  RUN_TEST(test_model_classification);
  RUN_TEST(test_recommended_ventilation);

  return UNITY_END();
}
