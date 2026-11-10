/**
 * @file iohome_replay_guard.cpp
 * @brief Rolling-code replay protection implementation
 * @author iown-homecontrol project
 */

#include "iohome_replay_guard.h"

#include <string.h>

namespace iohome {

ReplayGuard::ReplayGuard()
  : mac_count_(0),
    mac_next_(0),
    strict_(false),
    window_(DEFAULT_WINDOW),
    tick_(0),
    rejected_count_(0) {
  memset(entries_, 0, sizeof(entries_));
  memset(mac_ring_, 0, sizeof(mac_ring_));
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
  Entry* victim = nullptr;
  for (Entry& entry : entries_) {
    if (!entry.used) {
      victim = &entry;
      break;
    }
  }

  // Otherwise evict the least recently used entry that is not a known peer -
  // a flood of frames from made-up sources must not push a registered peer
  // (and its sequence) out of the table. Only when every slot is a known peer
  // does the LRU known peer go.
  if (victim == nullptr) {
    for (Entry& entry : entries_) {
      if (!entry.known && (victim == nullptr || entry.last_use < victim->last_use)) {
        victim = &entry;
      }
    }
  }
  if (victim == nullptr) {
    victim = &entries_[0];
    for (size_t i = 1; i < MAX_NODES; i++) {
      if (entries_[i].last_use < victim->last_use) {
        victim = &entries_[i];
      }
    }
  }

  // Wipe the evicted node's state; the newcomer must not inherit its sequence.
  memset(victim, 0, sizeof(Entry));
  memcpy(victim->node_id, node_id, NODE_ID_SIZE);
  victim->used = true;
  return victim;
}

ReplayGuard::Entry* ReplayGuard::lookup_for_accept(const uint8_t node_id[NODE_ID_SIZE]) {
  Entry* entry = find(node_id);
  if (entry != nullptr) {
    return entry;
  }
  if (strict_) {
    return nullptr;  // Unknown source: refused, and no slot spent on it.
  }
  return allocate(node_id);
}

bool ReplayGuard::sequence_ok(const Entry* entry, uint16_t sequence) const {
  if (entry == nullptr || !entry->seq_valid) {
    return true;  // Nothing heard or primed yet - nothing to compare against
  }

  // Modular distance from the last accepted value. Wrapping is handled for
  // free because the arithmetic is done on uint16_t.
  const uint16_t delta = static_cast<uint16_t>(sequence - entry->last_sequence);

  // delta == 0 is an exact replay. delta > window means the jump is too large
  // to be honest drift, which also stops an attacker from replaying a frame
  // captured long ago (its delta lands in the far side of the ring).
  return delta != 0 && delta <= (window_ == 0 ? 1 : window_);
}

bool ReplayGuard::mac_seen(const uint8_t mac[HMAC_SIZE]) const {
  for (size_t i = 0; i < mac_count_; i++) {
    if (memcmp(mac_ring_[i], mac, HMAC_SIZE) == 0) {
      return true;
    }
  }
  return false;
}

void ReplayGuard::remember_mac(const uint8_t mac[HMAC_SIZE]) {
  memcpy(mac_ring_[mac_next_], mac, HMAC_SIZE);
  mac_next_ = static_cast<uint8_t>((mac_next_ + 1) % GLOBAL_MAC_HISTORY);
  if (mac_count_ < GLOBAL_MAC_HISTORY) {
    mac_count_++;
  }
}

bool ReplayGuard::would_accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) const {
  if (node_id == nullptr) {
    return false;
  }

  const Entry* entry = find(node_id);
  if (entry == nullptr) {
    return !strict_;  // First frame from this node, unless strict mode forbids it
  }
  return sequence_ok(entry, sequence);
}

bool ReplayGuard::accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) {
  if (node_id == nullptr) {
    return false;
  }

  Entry* entry = lookup_for_accept(node_id);
  if (entry == nullptr || !sequence_ok(entry, sequence)) {
    rejected_count_++;
    return false;
  }

  entry->last_sequence = sequence;
  entry->seq_valid = true;
  entry->last_use = ++tick_;
  return true;
}

bool ReplayGuard::accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence,
                         const uint8_t mac[HMAC_SIZE]) {
  if (node_id == nullptr || mac == nullptr) {
    return false;
  }

  // Check both before recording either, so a rejected frame changes nothing.
  if (mac_seen(mac) || !would_accept(node_id, sequence)) {
    rejected_count_++;
    return false;
  }

  if (!accept(node_id, sequence)) {
    return false;  // Table could not hold the node; counted by accept()
  }
  remember_mac(mac);
  return true;
}

bool ReplayGuard::accept_mac(const uint8_t node_id[NODE_ID_SIZE], const uint8_t mac[HMAC_SIZE]) {
  if (node_id == nullptr || mac == nullptr) {
    return false;
  }

  if (mac_seen(mac)) {
    rejected_count_++;
    return false;
  }

  // Note the node for LRU/strict purposes, but do not invent a sequence for it:
  // seq_valid stays as it was, so a later 1W frame is judged on its own.
  Entry* entry = lookup_for_accept(node_id);
  if (entry == nullptr) {
    rejected_count_++;
    return false;
  }

  remember_mac(mac);
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
  entry->seq_valid = true;
  entry->known = true;
  entry->last_use = ++tick_;
  return true;
}

bool ReplayGuard::add_known_peer(const uint8_t node_id[NODE_ID_SIZE]) {
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

  entry->known = true;
  entry->last_use = ++tick_;
  return true;
}

bool ReplayGuard::last_sequence(const uint8_t node_id[NODE_ID_SIZE], uint16_t& sequence_out) const {
  if (node_id == nullptr) {
    return false;
  }

  const Entry* entry = find(node_id);
  if (entry == nullptr || !entry->seq_valid) {
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
  memset(mac_ring_, 0, sizeof(mac_ring_));
  mac_count_ = 0;
  mac_next_ = 0;
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
