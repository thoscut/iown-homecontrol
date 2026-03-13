/**
 * @file iohome_rolling_code_store.h
 * @brief Rolling code persistence for io-homecontrol 1W mode
 * @author iown-homecontrol project
 *
 * Persists rolling codes across reboots to prevent replay attacks
 * and ensure receivers accept commands after controller restart.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "iohome_constants.h"

namespace iohome {

/**
 * @brief Abstract interface for rolling code persistence
 */
class RollingCodeStore {
public:
  virtual ~RollingCodeStore() = default;

  /**
   * @brief Load rolling code for a given node
   * @param node_id Node ID (3 bytes)
   * @param code Output rolling code value (set to 0 if not found)
   * @return true if a stored value was found, false if not found
   */
  virtual bool load(const uint8_t node_id[NODE_ID_SIZE], uint16_t& code) = 0;

  /**
   * @brief Save rolling code for a given node
   * @param node_id Node ID (3 bytes)
   * @param code Rolling code value to persist
   * @return true on success
   */
  virtual bool save(const uint8_t node_id[NODE_ID_SIZE], uint16_t code) = 0;
};

/**
 * @brief In-memory rolling code store (no persistence, for testing/fallback)
 */
class MemoryRollingCodeStore : public RollingCodeStore {
public:
  bool load(const uint8_t node_id[NODE_ID_SIZE], uint16_t& code) override;
  bool save(const uint8_t node_id[NODE_ID_SIZE], uint16_t code) override;

private:
  static constexpr size_t MAX_ENTRIES = 16;
  struct Entry {
    uint8_t node_id[NODE_ID_SIZE];
    uint16_t code;
    bool used;
  };
  Entry entries_[MAX_ENTRIES] = {};
};

} // namespace iohome
