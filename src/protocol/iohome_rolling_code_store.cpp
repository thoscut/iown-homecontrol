/**
 * @file iohome_rolling_code_store.cpp
 * @brief In-memory rolling code store implementation
 * @author iown-homecontrol project
 */

#include "iohome_rolling_code_store.h"

namespace iohome {

bool MemoryRollingCodeStore::load(const uint8_t node_id[NODE_ID_SIZE], uint16_t& code) {
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (entries_[i].used && memcmp(entries_[i].node_id, node_id, NODE_ID_SIZE) == 0) {
      code = entries_[i].code;
      return true;
    }
  }
  code = 0;
  return false;
}

bool MemoryRollingCodeStore::save(const uint8_t node_id[NODE_ID_SIZE], uint16_t code) {
  // Update existing entry
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (entries_[i].used && memcmp(entries_[i].node_id, node_id, NODE_ID_SIZE) == 0) {
      entries_[i].code = code;
      return true;
    }
  }

  // Find a free slot
  for (size_t i = 0; i < MAX_ENTRIES; i++) {
    if (!entries_[i].used) {
      memcpy(entries_[i].node_id, node_id, NODE_ID_SIZE);
      entries_[i].code = code;
      entries_[i].used = true;
      return true;
    }
  }

  return false;  // No free slots
}

} // namespace iohome
