/**
 * @file iown_cover.h
 * @brief ESPHome cover entity for io-homecontrol blinds/shutters
 *
 * Provides OPEN, CLOSE, and STOP commands for io-homecontrol cover devices.
 * Position is estimated using timed travel durations (configurable).
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/cover/cover.h"
#include "iown_homecontrol.h"

namespace esphome {
namespace iown_homecontrol {

/**
 * @class IOWNCover
 * @brief Cover entity for io-homecontrol devices.
 *
 * Sends OPEN/CLOSE/STOP commands to a target io-homecontrol node.
 * Tracks position using timed estimation based on configured travel durations.
 */
class IOWNCover : public cover::Cover, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_hub(IOWNHomeControlComponent *hub) { this->hub_ = hub; }
  void set_target_address(uint32_t addr) { this->target_address_ = addr; }
  void set_open_duration(uint32_t ms) { this->open_duration_ms_ = ms; }
  void set_close_duration(uint32_t ms) { this->close_duration_ms_ = ms; }

  cover::CoverTraits get_traits() override;

 protected:
  void control(const cover::CoverCall &call) override;

  IOWNHomeControlComponent *hub_{nullptr};
  uint32_t target_address_{0x00003F};  // default: broadcast

  // Position tracking via timed estimation
  uint32_t open_duration_ms_{30000};   // Time for full OPEN travel
  uint32_t close_duration_ms_{30000};  // Time for full CLOSE travel
  float target_position_{-1.0f};       // Target: -1 = no movement in progress
  uint32_t movement_start_ms_{0};
  float start_position_{0.0f};         // Position when movement started
};

}  // namespace iown_homecontrol
}  // namespace esphome
