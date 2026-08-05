/**
 * @file iohome_replay_guard.h
 * @brief Rolling-code replay protection for received 1W frames
 * @author iown-homecontrol project
 *
 * Authenticated 1W frames carry a 16-bit sequence number ("rolling code") that
 * is folded into the MAC. Verifying the MAC alone does *not* stop an attacker
 * who simply records a valid frame off the air and re-transmits it later - the
 * MAC still checks out because nothing in it is bound to time.
 *
 * ReplayGuard closes that hole the way every rolling-code system does: it
 * remembers the highest sequence number seen per source node and only accepts
 * a frame whose sequence number moved forward, within a bounded window.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "iohome_constants.h"

namespace iohome {

/**
 * @brief Per-node replay protection
 *
 * Two mechanisms, because the two protocol modes carry different freshness
 * material:
 *
 *  - 1W frames carry a 16-bit sequence number, so accept() enforces forward
 *    progress within a bounded window.
 *  - 2W frames carry no sequence number; their freshness comes from the
 *    challenge negotiated at the start of a session. While that session is
 *    open the same challenge signs several frames, so an attacker who records
 *    one can re-transmit it and the MAC still verifies. accept_mac() closes
 *    that by remembering the MACs recently accepted from each node.
 *
 * Tracking is per source node ID and bounded by MAX_NODES; the least recently
 * used entry is evicted when the table is full.
 */
class ReplayGuard {
public:
  /// Number of source nodes tracked simultaneously.
  static constexpr size_t MAX_NODES = 16;

  /**
   * @brief Number of recent MACs remembered per node.
   *
   * io-homecontrol repeats each frame several times per packet, so the
   * history has to be deep enough to cover a burst and then some.
   */
  static constexpr size_t MAC_HISTORY = 4;

  /**
   * @brief Default forward window.
   *
   * A transmitter increments its counter for every frame it sends, including
   * frames this receiver never heard (out of range, collision, different
   * channel). The window bounds how far ahead a jump may be, so a replayed
   * *old* frame is still rejected while normal drift is tolerated.
   */
  static constexpr uint16_t DEFAULT_WINDOW = 1000;

  ReplayGuard();

  /**
   * @brief Check a received sequence number and record it on success
   *
   * @param node_id Source node ID of the frame (3 bytes)
   * @param sequence Sequence number taken from the frame
   * @return true if the frame is fresh and should be processed;
   *         false if it is a replay, stale, or too far ahead
   */
  bool accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence);

  /**
   * @brief Test a sequence number without recording it
   *
   * Useful when a frame still has to pass other checks before being accepted.
   */
  bool would_accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) const;

  /**
   * @brief Check a received MAC against the recent history and record it
   *
   * For 2W frames, which have no sequence number. Rejects a MAC this node has
   * produced within the last MAC_HISTORY accepted frames - that is either a
   * protocol-level repeat, which should only be acted on once, or a replay.
   *
   * This does not make 2W frames as fresh as 1W ones: an attacker who records
   * a frame and re-transmits it after MAC_HISTORY other frames from the same
   * node, within the same session, still gets through. The real bound is the
   * session timeout, after which the challenge changes and the MAC no longer
   * verifies. See docs/SECURITY-MODEL.md.
   *
   * @param node_id Source node ID of the frame (3 bytes)
   * @param mac MAC taken from the frame (6 bytes)
   * @return true if the MAC has not been seen recently
   */
  bool accept_mac(const uint8_t node_id[NODE_ID_SIZE], const uint8_t mac[HMAC_SIZE]);

  /**
   * @brief Seed or override the last known sequence number for a node
   *
   * Call this after loading a persisted counter, or when re-pairing a device.
   *
   * @return false if the table is full and no entry could be evicted
   */
  bool prime(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence);

  /**
   * @brief Look up the last accepted sequence number for a node
   *
   * @param node_id Source node ID (3 bytes)
   * @param sequence_out Receives the stored value when the node is known
   * @return true if the node is being tracked
   */
  bool last_sequence(const uint8_t node_id[NODE_ID_SIZE], uint16_t& sequence_out) const;

  /// Forget a single node (e.g. after unpairing).
  bool forget(const uint8_t node_id[NODE_ID_SIZE]);

  /// Forget every tracked node.
  void reset();

  /// Number of nodes currently tracked.
  size_t tracked_count() const;

  /// Configure the forward window. A window of 0 accepts only sequence+1.
  void set_window(uint16_t window) { window_ = window; }
  uint16_t get_window() const { return window_; }

  /// Number of frames rejected as replays since construction/reset.
  uint32_t rejected_count() const { return rejected_count_; }

private:
  struct Entry {
    uint8_t node_id[NODE_ID_SIZE];
    uint16_t last_sequence;
    uint32_t last_use;   // monotonic tick for LRU eviction
    bool used;

    // Ring of recently accepted MACs, for 2W frames.
    uint8_t mac_history[MAC_HISTORY][HMAC_SIZE];
    uint8_t mac_count;   // how many slots hold a MAC
    uint8_t mac_next;    // next slot to overwrite
  };

  Entry* find(const uint8_t node_id[NODE_ID_SIZE]);
  const Entry* find(const uint8_t node_id[NODE_ID_SIZE]) const;
  Entry* allocate(const uint8_t node_id[NODE_ID_SIZE]);

  Entry entries_[MAX_NODES];
  uint16_t window_;
  uint32_t tick_;
  uint32_t rejected_count_;
};

} // namespace iohome
