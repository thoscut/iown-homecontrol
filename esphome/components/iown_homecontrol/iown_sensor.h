/**
 * @file iown_sensor.h
 * @brief Diagnostic sensors for the io-homecontrol hub
 *
 * Publishes link quality and receive-path counters so a flaky installation can
 * be diagnosed from Home Assistant rather than the serial log.
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "iown_homecontrol.h"

namespace esphome {
namespace iown_homecontrol {

/**
 * @class IOWNSensor
 * @brief Polls the hub and publishes its diagnostic counters.
 */
class IOWNSensor : public PollingComponent {
 public:
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_hub(IOWNHomeControlComponent *hub) { this->hub_ = hub; }
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_sensor_ = s; }
  void set_frames_received_sensor(sensor::Sensor *s) { this->frames_received_sensor_ = s; }
  void set_crc_errors_sensor(sensor::Sensor *s) { this->crc_errors_sensor_ = s; }
  void set_rolling_code_sensor(sensor::Sensor *s) { this->rolling_code_sensor_ = s; }

 protected:
  IOWNHomeControlComponent *hub_{nullptr};
  sensor::Sensor *rssi_sensor_{nullptr};
  sensor::Sensor *frames_received_sensor_{nullptr};
  sensor::Sensor *crc_errors_sensor_{nullptr};
  sensor::Sensor *rolling_code_sensor_{nullptr};
};

}  // namespace iown_homecontrol
}  // namespace esphome
