/**
 * @file iohome_replay_guard.cpp
 * @brief Rolling-code replay protection implementation
 * @author iown-homecontrol project
 */

#include "iohome_replay_guard.h"

#include <string.h>

namespace iohome {

ReplayGuard::ReplayGuard()
  : window_(DEFAULT_WINDOW),
    tick_(0),
    rejected_count_(0) {
  memset(entries_, 0, sizeof(entries_));
}

ReplayGuard::Entry* ReplayGuard::find(const uint8_t node_id[NODE_ID_SIZE]) {
  for (Entry& entry : entries_) {
    if (entry.used && memcmp(entry.node_id, node_id, NODE_ID_SIZE) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

const ReplayGuard::Entry* ReplayGuard::find(const uint8_t node_id[NODE_ID_SIZE]) const {
  for (const Entry& entry : entries_) {
    if (entry.used && memcmp(entry.node_id, node_id, NODE_ID_SIZE) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

ReplayGuard::Entry* ReplayGuard::allocate(const uint8_t node_id[NODE_ID_SIZE]) {
  // Prefer a free slot.
  for (Entry& entry : entries_) {
    if (!entry.used) {
      memset(&entry, 0, sizeof(Entry));
      memcpy(entry.node_id, node_id, NODE_ID_SIZE);
      entry.used = true;
      return &entry;
    }
  }

  // Otherwise evict the least recently used entry.
  Entry* victim = &entries_[0];
  for (size_t i = 1; i < MAX_NODES; i++) {
    if (entries_[i].last_use < victim->last_use) {
      victim = &entries_[i];
    }
  }

  // Wipe the evicted node's state; the newcomer must not inherit its sequence
  // number or its MAC history.
  memset(victim, 0, sizeof(Entry));
  memcpy(victim->node_id, node_id, NODE_ID_SIZE);
  victim->used = true;
  return victim;
}

bool ReplayGuard::would_accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) const {
  if (node_id == nullptr) {
    return false;
  }

  const Entry* entry = find(node_id);
  if (entry == nullptr) {
    return true; // First frame from this node - nothing to compare against
  }

  // Modular distance from the last accepted value. Wrapping is handled for
  // free because the arithmetic is done on uint16_t.
  const uint16_t delta = static_cast<uint16_t>(sequence - entry->last_sequence);

  // delta == 0 is an exact replay. delta > window means the jump is too large
  // to be honest drift, which also stops an attacker from replaying a frame
  // captured long ago (its delta lands in the far side of the ring).
  return delta != 0 && delta <= (window_ == 0 ? 1 : window_);
}

bool ReplayGuard::accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) {
  if (node_id == nullptr) {
    return false;
  }

  Entry* entry = find(node_id);

  if (entry == nullptr) {
    entry = allocate(node_id);
    if (entry == nullptr) {
      return false;
    }
    entry->last_sequence = sequence;
    entry->last_use = ++tick_;
    return true;
  }

  const uint16_t delta = static_cast<uint16_t>(sequence - entry->last_sequence);
  if (delta == 0 || delta > (window_ == 0 ? 1 : window_)) {
    rejected_count_++;
    return false;
  }

  entry->last_sequence = sequence;
  entry->last_use = ++tick_;
  return true;
}

bool ReplayGuard::accept_mac(const uint8_t node_id[NODE_ID_SIZE], const uint8_t mac[HMAC_SIZE]) {
  if (node_id == nullptr || mac == nullptr) {
    return false;
  }

  Entry* entry = find(node_id);
  if (entry == nullptr) {
    entry = allocate(node_id);
    if (entry == nullptr) {
      return false;
    }
  }

  for (size_t i = 0; i < entry->mac_count; i++) {
    if (memcmp(entry->mac_history[i], mac, HMAC_SIZE) == 0) {
      rejected_count_++;
      return false;
    }
  }

  memcpy(entry->mac_history[entry->mac_next], mac, HMAC_SIZE);
  entry->mac_next = static_cast<uint8_t>((entry->mac_next + 1) % MAC_HISTORY);
  if (entry->mac_count < MAC_HISTORY) {
    entry->mac_count++;
  }

  entry->last_use = ++tick_;
  return true;
}

bool ReplayGuard::prime(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) {
  if (node_id == nullptr) {
    return false;
  }

  Entry* entry = find(node_id);
  if (entry == nullptr) {
    entry = allocate(node_id);
    if (entry == nullptr) {
      return false;
    }
  }

  entry->last_sequence = sequence;
  entry->last_use = ++tick_;
  return true;
}

bool ReplayGuard::last_sequence(const uint8_t node_id[NODE_ID_SIZE], uint16_t& sequence_out) const {
  if (node_id == nullptr) {
    return false;
  }

  const Entry* entry = find(node_id);
  if (entry == nullptr) {
    return false;
  }

  sequence_out = entry->last_sequence;
  return true;
}

bool ReplayGuard::forget(const uint8_t node_id[NODE_ID_SIZE]) {
  if (node_id == nullptr) {
    return false;
  }

  Entry* entry = find(node_id);
  if (entry == nullptr) {
    return false;
  }

  memset(entry, 0, sizeof(Entry));
  return true;
}

void ReplayGuard::reset() {
  memset(entries_, 0, sizeof(entries_));
  tick_ = 0;
  rejected_count_ = 0;
}

size_t ReplayGuard::tracked_count() const {
  size_t count = 0;
  for (const Entry& entry : entries_) {
    if (entry.used) {
      count++;
    }
  }
  return count;
}

} // namespace iohome
