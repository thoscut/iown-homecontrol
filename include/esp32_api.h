/**
  * @file    esp32_api.h
  * @author  iown-homecontrol
  * @brief   ESP32 API functions
  */

#pragma once

/*
 * `#pragma once` belongs above the includes: <Arduino.h> sat before it and was
 * re-processed on every inclusion.
 *
 * The ESP-IDF headers are guarded because they only exist in an ESP-IDF build.
 * Unguarded, including this header anywhere else failed on a missing
 * esp_system.h instead of compiling to nothing, which is what a
 * platform-specific header should do off-platform.
 */

#if defined(ESP32) || defined(ESP_PLATFORM)

#include <Arduino.h>

#ifdef __cplusplus
  extern "C" {
#endif

#include "esp_system.h" // Peripheral configuration in the ESP system
#include "sdkconfig.h"

// #include "esp32_api_spi.h"

#ifdef __cplusplus
}
#endif

#endif  // ESP32 || ESP_PLATFORM
