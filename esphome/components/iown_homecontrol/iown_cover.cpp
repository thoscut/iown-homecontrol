/**
 * @file iown_cover.cpp
 * @brief ESPHome cover entity for io-homecontrol - implementation
 *
 * Position is tracked using timed estimation: the cover interpolates its
 * position between start and target based on elapsed time and the configured
 * full-travel durations.  For intermediate targets the cover automatically
 * sends a STOP command when the estimated position is reached.
 */

#include "iown_cover.h"

#include <cmath>

namespace esphome {
namespace iown_homecontrol {

static const char *const TAG = "iown_homecontrol.cover";

void IOWNCover::setup() {
  if (this->hub_ != nullptr) {
    this->hub_->register_cover(this);
  }
  // Set initial state to fully open
  this->position = cover::COVER_OPEN;
  this->publish_state();
}

void IOWNCover::loop() {
  if (this->target_position_ < 0.0f) {
    return;  // No movement in progress
  }

  uint32_t now = millis();
  uint32_t elapsed = now - this->movement_start_ms_;

  bool opening = this->target_position_ > this->start_position_;
  uint32_t duration = opening ? this->open_duration_ms_ : this->close_duration_ms_;
  float travel_distance = std::abs(this->target_position_ - this->start_position_);
  uint32_t expected_duration = static_cast<uint32_t>(duration * travel_distance);

  if (elapsed >= expected_duration) {
    // Movement complete
    this->position = this->target_position_;

    // For intermediate targets, send STOP to halt the physical motor
    if (this->position > 0.01f && this->position < 0.99f) {
      if (this->hub_ != nullptr) {
        this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_STOP);
      }
    }

    this->target_position_ = -1.0f;
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->publish_state();
    return;
  }

  // Interpolate position based on elapsed time
  float progress = static_cast<float>(elapsed) / static_cast<float>(expected_duration);
  if (opening) {
    this->position = this->start_position_ + progress * travel_distance;
  } else {
    this->position = this->start_position_ - progress * travel_distance;
  }

  // Clamp to valid range
  if (this->position < 0.0f)
    this->position = 0.0f;
  if (this->position > 1.0f)
    this->position = 1.0f;

  this->publish_state();
}

void IOWNCover::dump_config() {
  LOG_COVER("", "io-homecontrol Cover", this);
  ESP_LOGCONFIG(TAG, "  Target Address: 0x%06X",
                static_cast<unsigned int>(this->target_address_));
  ESP_LOGCONFIG(TAG, "  Open Duration: %u ms",
                static_cast<unsigned int>(this->open_duration_ms_));
  ESP_LOGCONFIG(TAG, "  Close Duration: %u ms",
                static_cast<unsigned int>(this->close_duration_ms_));
}

cover::CoverTraits IOWNCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_stop(true);
  traits.set_supports_position(true);
  traits.set_supports_tilt(false);
  traits.set_is_assumed_state(false);
  return traits;
}

void IOWNCover::control(const cover::CoverCall &call) {
  if (this->hub_ == nullptr) {
    ESP_LOGE(TAG, "Hub not configured");
    return;
  }

  if (call.get_stop()) {
    ESP_LOGI(TAG, "STOP -> 0x%06X", static_cast<unsigned int>(this->target_address_));
    this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_STOP);
    // Stop at current estimated position
    this->target_position_ = -1.0f;
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->publish_state();
    return;
  }

  if (call.get_position().has_value()) {
    float pos = *call.get_position();

    this->start_position_ = this->position;
    this->movement_start_ms_ = millis();

    if (pos >= 0.99f) {
      // Full open
      ESP_LOGI(TAG, "OPEN -> 0x%06X", static_cast<unsigned int>(this->target_address_));
      this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_OPEN);
      this->target_position_ = cover::COVER_OPEN;
      this->current_operation = cover::COVER_OPERATION_OPENING;
    } else if (pos <= 0.01f) {
      // Full close
      ESP_LOGI(TAG, "CLOSE -> 0x%06X", static_cast<unsigned int>(this->target_address_));
      this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_CLOSE);
      this->target_position_ = cover::COVER_CLOSED;
      this->current_operation = cover::COVER_OPERATION_CLOSING;
    } else {
      // Intermediate position – start moving and let loop() send STOP at the right time
      if (pos > this->position) {
        ESP_LOGI(TAG, "OPEN to %.0f%% -> 0x%06X", pos * 100,
                 static_cast<unsigned int>(this->target_address_));
        this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_OPEN);
        this->current_operation = cover::COVER_OPERATION_OPENING;
      } else {
        ESP_LOGI(TAG, "CLOSE to %.0f%% -> 0x%06X", pos * 100,
                 static_cast<unsigned int>(this->target_address_));
        this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_CLOSE);
        this->current_operation = cover::COVER_OPERATION_CLOSING;
      }
      this->target_position_ = pos;
    }

    this->publish_state();
  }
}

}  // namespace iown_homecontrol
}  // namespace esphome
