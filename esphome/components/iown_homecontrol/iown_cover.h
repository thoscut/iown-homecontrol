/**
 * @file iown_cover.h
 * @brief ESPHome cover entity for io-homecontrol blinds/shutters
 *
 * Sends OPEN, CLOSE, STOP and position commands to an io-homecontrol node, and
 * optionally a tilt angle for slatted products.
 *
 * Position is estimated from the configured travel durations. io-homecontrol
 * actuators do report their position, but only over an authenticated 2W
 * session; until that is wired up the estimate is the best a 1W controller can
 * do, and `assumed_state` reflects that honestly in Home Assistant.
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/cover/cover.h"
#include "iown_homecontrol.h"

namespace esphome {
namespace iown_homecontrol {

/**
 * @brief What position a cover assumes when the device boots.
 */
enum CoverRestoreMode : uint8_t {
  COVER_RESTORE_NONE = 0,    // Start at the configured initial position
  COVER_RESTORE_FROM_NVS = 1  // Restore the last published position
};

/**
 * @class IOWNCover
 * @brief Cover entity for io-homecontrol devices.
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
  void set_supports_tilt(bool supports) { this->supports_tilt_ = supports; }
  void set_assumed_state(bool assumed) { this->assumed_state_ = assumed; }
  void set_restore_mode(CoverRestoreMode mode) { this->restore_mode_ = mode; }
  void set_initial_position(float position) { this->initial_position_ = position; }

  cover::CoverTraits get_traits() override;

  /**
   * @brief Feed a position report received from the actuator.
   *
   * Ignored unless `src_address` matches this cover's target.
   *
   * @param src_address Source node address of the reporting frame
   * @param percent_closed 0 = fully open, 100 = fully closed
   */
  void on_position_report(uint32_t src_address, uint8_t percent_closed);

 protected:
  void control(const cover::CoverCall &call) override;

  /** Send a main-parameter command to the target node. */
  bool send_main_param_(uint16_t main_param, uint8_t fp1 = 0x00);

  /**
   * @brief Compute the timed position estimate without side effects.
   *
   * @param now Current millis()
   * @param arrived Set to true when the movement has run its full duration
   */
  float estimate_position_(uint32_t now, bool *arrived) const;

  /** Advance the timed position estimate; returns true if it should be published. */
  bool update_estimate_(uint32_t now);

  /** Stop any movement in progress and settle at `position`. */
  void finish_movement_();

  IOWNHomeControlComponent *hub_{nullptr};
  uint32_t target_address_{0x00003F};  // default: broadcast

  // Position tracking via timed estimation
  uint32_t open_duration_ms_{30000};   // Time for a full OPEN travel
  uint32_t close_duration_ms_{30000};  // Time for a full CLOSE travel
  float target_position_{-1.0f};       // -1 = no movement in progress
  uint32_t movement_start_ms_{0};
  float start_position_{0.0f};
  uint32_t last_publish_ms_{0};

  bool supports_tilt_{false};
  bool assumed_state_{true};
  CoverRestoreMode restore_mode_{COVER_RESTORE_NONE};
  float initial_position_{cover::COVER_OPEN};

  /// Minimum interval between state publishes while moving, in milliseconds.
  static const uint32_t PUBLISH_INTERVAL_MS = 1000;
};

}  // namespace iown_homecontrol
}  // namespace esphome
