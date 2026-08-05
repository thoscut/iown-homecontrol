/**
 * @file iown_cover.cpp
 * @brief ESPHome cover entity for io-homecontrol - implementation
 */

#include "iown_cover.h"

#include <cmath>

namespace esphome {
namespace iown_homecontrol {

static const char *const TAG = "iown_homecontrol.cover";

void IOWNCover::setup() {
  if (this->hub_ != nullptr) {
    this->hub_->register_cover(this);
  } else {
    ESP_LOGE(TAG, "No hub configured");
    this->mark_failed();
    return;
  }

  this->position = this->initial_position_;

  if (this->restore_mode_ == COVER_RESTORE_FROM_NVS) {
    auto restore = this->restore_state_();
    if (restore.has_value()) {
      restore->apply(this);
      // A restored position is an estimate, not a reading: never resume a
      // movement the actuator is not actually performing.
      this->target_position_ = -1.0f;
      this->current_operation = cover::COVER_OPERATION_IDLE;
      return;
    }
  }

  this->publish_state();
}

void IOWNCover::loop() {
  if (this->target_position_ < 0.0f) {
    return;  // No movement in progress
  }

  const uint32_t now = millis();
  if (this->update_estimate_(now)) {
    this->publish_state();
    this->last_publish_ms_ = now;
  }
}

bool IOWNCover::update_estimate_(uint32_t now) {
  const uint32_t elapsed = now - this->movement_start_ms_;

  const bool opening = this->target_position_ > this->start_position_;
  const uint32_t duration = opening ? this->open_duration_ms_ : this->close_duration_ms_;
  const float travel_distance = std::fabs(this->target_position_ - this->start_position_);
  const uint32_t expected_duration = static_cast<uint32_t>(duration * travel_distance);

  if (elapsed >= expected_duration) {
    this->position = this->target_position_;

    // For an intermediate target the actuator is still running: it was told to
    // travel all the way, so it needs an explicit STOP at the right moment.
    if (this->position > 0.01f && this->position < 0.99f) {
      this->send_main_param_(IOHC_PARAM_STOP);
    }

    this->finish_movement_();
    return true;
  }

  const float progress = static_cast<float>(elapsed) / static_cast<float>(expected_duration);
  float estimate = opening ? (this->start_position_ + progress * travel_distance)
                           : (this->start_position_ - progress * travel_distance);

  estimate = clamp(estimate, 0.0f, 1.0f);

  const bool changed = std::fabs(estimate - this->position) > 0.001f;
  this->position = estimate;

  // Publishing on every loop iteration floods the API; once a second while
  // moving is plenty for a cover that takes tens of seconds to travel.
  return changed && (now - this->last_publish_ms_ >= PUBLISH_INTERVAL_MS);
}

void IOWNCover::finish_movement_() {
  this->target_position_ = -1.0f;
  this->current_operation = cover::COVER_OPERATION_IDLE;
}

void IOWNCover::dump_config() {
  LOG_COVER("", "io-homecontrol Cover", this);
  ESP_LOGCONFIG(TAG, "  Target Address: 0x%06X", static_cast<unsigned int>(this->target_address_));
  ESP_LOGCONFIG(TAG, "  Open Duration: %u ms", static_cast<unsigned int>(this->open_duration_ms_));
  ESP_LOGCONFIG(TAG, "  Close Duration: %u ms", static_cast<unsigned int>(this->close_duration_ms_));
  ESP_LOGCONFIG(TAG, "  Tilt: %s", this->supports_tilt_ ? "supported" : "not supported");
  ESP_LOGCONFIG(TAG, "  Assumed state: %s", YESNO(this->assumed_state_));
}

cover::CoverTraits IOWNCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_stop(true);
  traits.set_supports_position(true);
  traits.set_supports_tilt(this->supports_tilt_);
  // The position is a timed estimate unless the actuator reports back, so
  // advertise it as assumed rather than claiming a reading we do not have.
  traits.set_is_assumed_state(this->assumed_state_);
  return traits;
}

bool IOWNCover::send_main_param_(uint16_t main_param, uint8_t fp1) {
  if (this->hub_ == nullptr) {
    return false;
  }
  return this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, main_param, fp1);
}

void IOWNCover::on_position_report(uint32_t src_address, uint8_t percent_closed) {
  if (src_address != this->target_address_) {
    return;
  }

  if (percent_closed > 100) {
    return;
  }

  // ESPHome counts openness: 1.0 is fully open, the protocol counts closure.
  const float reported = 1.0f - (static_cast<float>(percent_closed) / 100.0f);

  ESP_LOGD(TAG, "Position report from 0x%06X: %.0f%% open",
           static_cast<unsigned int>(src_address), reported * 100.0f);

  this->position = clamp(reported, 0.0f, 1.0f);

  // A report supersedes the estimate: the actuator has settled.
  this->finish_movement_();
  this->publish_state();
}

void IOWNCover::control(const cover::CoverCall &call) {
  if (this->hub_ == nullptr) {
    ESP_LOGE(TAG, "Hub not configured");
    return;
  }

  if (call.get_stop()) {
    ESP_LOGI(TAG, "STOP -> 0x%06X", static_cast<unsigned int>(this->target_address_));
    this->send_main_param_(IOHC_PARAM_STOP);

    // Freeze at the current estimate.
    if (this->target_position_ >= 0.0f) {
      this->update_estimate_(millis());
    }
    this->finish_movement_();
    this->publish_state();
    return;
  }

  if (call.get_tilt().has_value()) {
    const float tilt = clamp(*call.get_tilt(), 0.0f, 1.0f);
    ESP_LOGI(TAG, "TILT %.0f%% -> 0x%06X", tilt * 100.0f,
             static_cast<unsigned int>(this->target_address_));

    // Tilt travels in Functional Parameter 1 on the 0..0xC8 scale while the
    // main parameter holds "current position", so the slats move but the
    // cover does not.
    const uint8_t fp1 = static_cast<uint8_t>(tilt * 200.0f);
    if (this->send_main_param_(IOHC_PARAM_STOP, fp1)) {
      this->tilt = tilt;
      this->publish_state();
    }
    return;
  }

  if (!call.get_position().has_value()) {
    return;
  }

  const float pos = clamp(*call.get_position(), 0.0f, 1.0f);

  this->start_position_ = this->position;
  this->movement_start_ms_ = millis();
  this->last_publish_ms_ = this->movement_start_ms_;

  if (pos >= 0.99f) {
    ESP_LOGI(TAG, "OPEN -> 0x%06X", static_cast<unsigned int>(this->target_address_));
    this->send_main_param_(IOHC_PARAM_OPEN);
    this->target_position_ = cover::COVER_OPEN;
    this->current_operation = cover::COVER_OPERATION_OPENING;
  } else if (pos <= 0.01f) {
    ESP_LOGI(TAG, "CLOSE -> 0x%06X", static_cast<unsigned int>(this->target_address_));
    this->send_main_param_(IOHC_PARAM_CLOSE);
    this->target_position_ = cover::COVER_CLOSED;
    this->current_operation = cover::COVER_OPERATION_CLOSING;
  } else {
    // Ask the actuator for the position directly. Actuators that honour a
    // percentage stop on their own; for the rest, loop() sends STOP once the
    // timed estimate reaches the target.
    const uint8_t percent_closed = static_cast<uint8_t>((1.0f - pos) * 100.0f + 0.5f);
    const uint16_t main_param =
        IOWNHomeControlComponent::main_param_from_percent_closed(percent_closed);

    ESP_LOGI(TAG, "POSITION %.0f%% -> 0x%06X (main=0x%04X)", pos * 100.0f,
             static_cast<unsigned int>(this->target_address_), main_param);

    this->send_main_param_(main_param);
    this->target_position_ = pos;
    this->current_operation =
        (pos > this->position) ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
  }

  if (std::fabs(this->target_position_ - this->position) < 0.001f) {
    // Already there; nothing to time.
    this->finish_movement_();
  }

  this->publish_state();
}

}  // namespace iown_homecontrol
}  // namespace esphome
