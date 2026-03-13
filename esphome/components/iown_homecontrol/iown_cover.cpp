/**
 * @file iown_cover.cpp
 * @brief ESPHome cover entity for io-homecontrol - implementation
 */

#include "iown_cover.h"

namespace esphome {
namespace iown_homecontrol {

static const char *const TAG = "iown_homecontrol.cover";

void IOWNCover::setup() {
  if (this->hub_ != nullptr) {
    this->hub_->register_cover(this);
  }
  // Set initial assumed state
  this->position = cover::COVER_OPEN;
  this->publish_state();
}

void IOWNCover::dump_config() {
  LOG_COVER("", "io-homecontrol Cover", this);
  ESP_LOGCONFIG(TAG, "  Target Address: 0x%06X",
                static_cast<unsigned int>(this->target_address_));
}

cover::CoverTraits IOWNCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_stop(true);
  traits.set_supports_position(false);
  traits.set_supports_tilt(false);
  traits.set_is_assumed_state(true);
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
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->publish_state();
    return;
  }

  if (call.get_position().has_value()) {
    float pos = *call.get_position();

    if (pos == cover::COVER_OPEN) {
      ESP_LOGI(TAG, "OPEN -> 0x%06X", static_cast<unsigned int>(this->target_address_));
      this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_OPEN);
      this->position = cover::COVER_OPEN;
      this->current_operation = cover::COVER_OPERATION_OPENING;
    } else if (pos == cover::COVER_CLOSED) {
      ESP_LOGI(TAG, "CLOSE -> 0x%06X", static_cast<unsigned int>(this->target_address_));
      this->hub_->send_cover_command(this->target_address_, IOHC_CMD_EXECUTE, IOHC_PARAM_CLOSE);
      this->position = cover::COVER_CLOSED;
      this->current_operation = cover::COVER_OPERATION_CLOSING;
    }

    this->publish_state();
  }
}

}  // namespace iown_homecontrol
}  // namespace esphome
