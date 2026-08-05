/**
 * @file iown_sensor.cpp
 * @brief Diagnostic sensors for the io-homecontrol hub - implementation
 */

#include "iown_sensor.h"

namespace esphome {
namespace iown_homecontrol {

static const char *const TAG = "iown_homecontrol.sensor";

void IOWNSensor::update() {
  if (this->hub_ == nullptr) {
    return;
  }

  if (this->rssi_sensor_ != nullptr) {
    this->rssi_sensor_->publish_state(this->hub_->last_rssi());
  }
  if (this->frames_received_sensor_ != nullptr) {
    this->frames_received_sensor_->publish_state(this->hub_->frames_received());
  }
  if (this->crc_errors_sensor_ != nullptr) {
    this->crc_errors_sensor_->publish_state(this->hub_->crc_errors());
  }
  if (this->rolling_code_sensor_ != nullptr) {
    this->rolling_code_sensor_->publish_state(this->hub_->rolling_code());
  }
}

void IOWNSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "io-homecontrol diagnostics:");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "RSSI", this->rssi_sensor_);
  LOG_SENSOR("  ", "Frames received", this->frames_received_sensor_);
  LOG_SENSOR("  ", "CRC errors", this->crc_errors_sensor_);
  LOG_SENSOR("  ", "Rolling code", this->rolling_code_sensor_);
}

}  // namespace iown_homecontrol
}  // namespace esphome
