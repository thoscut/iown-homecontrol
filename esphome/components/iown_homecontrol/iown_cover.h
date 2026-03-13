/**
 * @file iown_cover.h
 * @brief ESPHome cover entity for io-homecontrol blinds/shutters
 *
 * Provides OPEN, CLOSE, and STOP commands for io-homecontrol cover devices.
 * Uses assumed state since position feedback requires 2W protocol handshake.
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
 * Position tracking is assumed (no feedback from device in current implementation).
 */
class IOWNCover : public cover::Cover, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_hub(IOWNHomeControlComponent *hub) { this->hub_ = hub; }
  void set_target_address(uint32_t addr) { this->target_address_ = addr; }

  cover::CoverTraits get_traits() override;

 protected:
  void control(const cover::CoverCall &call) override;

  IOWNHomeControlComponent *hub_{nullptr};
  uint32_t target_address_{0x00003F};  // default: broadcast
};

}  // namespace iown_homecontrol
}  // namespace esphome
