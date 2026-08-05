/**
 * @file iohome_rolling_code_store.cpp
 * @brief In-memory rolling code store implementation
 * @author iown-homecontrol project
 */

#include "iohome_rolling_code_store.h"

namespace iohome {

bool MemoryRollingCodeStore::load(const uint8_t node_id[NODE_ID_SIZE], uint16_t& code) {
  for (const Entry& entry : entries_) {
    if (entry.used && memcmp(entry.node_id, node_id, NODE_ID_SIZE) == 0) {
      code = entry.code;
      return true;
    }
  }
  code = 0;
  return false;
}

bool MemoryRollingCodeStore::save(const uint8_t node_id[NODE_ID_SIZE], uint16_t code) {
  // Update existing entry
  for (Entry& entry : entries_) {
    if (entry.used && memcmp(entry.node_id, node_id, NODE_ID_SIZE) == 0) {
      entry.code = code;
      return true;
    }
  }

  // Find a free slot
  for (Entry& entry : entries_) {
    if (!entry.used) {
      memcpy(entry.node_id, node_id, NODE_ID_SIZE);
      entry.code = code;
      entry.used = true;
      return true;
    }
  }

  return false;  // No free slots
}

} // namespace iohome
