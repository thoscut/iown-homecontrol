/**
 * @file iohome_nvs_store.h
 * @brief ESP32 NVS-backed rolling code store
 * @author iown-homecontrol project
 */

#pragma once

#ifdef ARDUINO

#include "iohome_rolling_code_store.h"
#include <Preferences.h>

namespace iohome {

/**
 * @brief NVS-backed rolling code store for ESP32
 *
 * Stores rolling codes in ESP32 Non-Volatile Storage using the
 * Arduino Preferences library. Each node ID maps to a key string.
 */
class NvsRollingCodeStore : public RollingCodeStore {
public:
  /**
   * @brief Construct NVS store
   * @param ns_name NVS namespace name (max 15 chars)
   */
  explicit NvsRollingCodeStore(const char* ns_name = "iohome_rc");

  bool load(const uint8_t node_id[NODE_ID_SIZE], uint16_t& code) override;
  bool save(const uint8_t node_id[NODE_ID_SIZE], uint16_t code) override;

private:
  const char* ns_name_;
  /** Convert node_id to NVS key string (e.g., "1A380B") */
  void node_id_to_key(const uint8_t node_id[NODE_ID_SIZE], char key[7]);
};

} // namespace iohome

#endif // ARDUINO
