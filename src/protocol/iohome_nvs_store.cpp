/**
 * @file iohome_nvs_store.cpp
 * @brief ESP32 NVS-backed rolling code store implementation
 * @author iown-homecontrol project
 */

#ifdef ARDUINO

#include "iohome_nvs_store.h"
#include <stdio.h>

namespace iohome {

NvsRollingCodeStore::NvsRollingCodeStore(const char* ns_name)
  : ns_name_(ns_name)
{
}

void NvsRollingCodeStore::node_id_to_key(const uint8_t node_id[NODE_ID_SIZE], char key[7]) {
  snprintf(key, 7, "%02X%02X%02X", node_id[0], node_id[1], node_id[2]);
}

bool NvsRollingCodeStore::load(const uint8_t node_id[NODE_ID_SIZE], uint16_t& code) {
  char key[7];
  node_id_to_key(node_id, key);

  Preferences prefs;
  if (!prefs.begin(ns_name_, true)) {  // read-only
    code = 0;
    return false;
  }

  bool found = prefs.isKey(key);
  if (found) {
    code = prefs.getUShort(key, 0);
  } else {
    code = 0;
  }

  prefs.end();
  return found;
}

bool NvsRollingCodeStore::save(const uint8_t node_id[NODE_ID_SIZE], uint16_t code) {
  char key[7];
  node_id_to_key(node_id, key);

  Preferences prefs;
  if (!prefs.begin(ns_name_, false)) {  // read-write
    return false;
  }

  size_t written = prefs.putUShort(key, code);
  prefs.end();
  return written == sizeof(uint16_t);
}

} // namespace iohome

#endif // ARDUINO
