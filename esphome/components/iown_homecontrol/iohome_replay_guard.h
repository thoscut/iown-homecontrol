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
 *  - Frames without a sequence number go through accept_mac(), which only
 *    rejects a MAC accepted recently. (Spec-conformant 2W frames carry no MAC
 *    at all; their freshness is the 0x3C/0x3D exchange with a fresh nonce.)
 *
 * Sequence tracking is per source node ID and bounded by MAX_NODES; the least
 * recently used entry is evicted when the table is full. The MAC history is
 * global across nodes.
 *
 * Persistence: this state lives in RAM only. It is NOT saved across a reboot.
 * Unless a node is primed (prime()) its first authenticated frame after a
 * power-cycle is accepted at whatever sequence it carries, because there is no
 * previous value to compare against.
 *
 * Two things bound what an attacker can do with that:
 *
 *  - The MAC does not cover the frame's addresses, so a recorded frame can be
 *    re-addressed to a source this guard has never seen (and the CRC
 *    recomputed). Keying on the source alone would accept that rewrite as a
 *    "first frame" every time, without limit. A global ring of recently
 *    accepted MACs - not keyed by source - rejects a MAC that was already
 *    accepted from *any* node, whatever address it now claims.
 *  - Strict mode (set_strict(true)) accepts frames only from sources that were
 *    registered with prime() or add_known_peer(); everything else is rejected
 *    without allocating an entry. That stops the unknown-source path entirely.
 *
 * What remains: a known peer that has not been heard since boot (and was not
 * primed with a persisted sequence) accepts its first frame at any sequence,
 * and the MAC ring is finite (GLOBAL_MAC_HISTORY). A receiver that acts on 1W
 * commands - rather than only reporting position, which is all this project
 * does with them - must persist the last-accepted sequence per peer and restore
 * it with prime() in begin(). See docs/SECURITY-MODEL.md.
 */
class ReplayGuard {
public:
  /// Number of source nodes tracked simultaneously.
  static constexpr size_t MAX_NODES = 16;

  /**
   * @brief Number of recently accepted MACs remembered, across all nodes.
   *
   * Not keyed by source: the MAC does not cover the addresses, so a replay
   * re-addressed to another source carries the same MAC. io-homecontrol also
   * repeats each frame several times per packet, so the ring has to be deep
   * enough to cover a burst from every active node and then some.
   */
  static constexpr size_t GLOBAL_MAC_HISTORY = 32;

  /// @deprecated The MAC history is global now; kept for source compatibility.
  static constexpr size_t MAC_HISTORY = GLOBAL_MAC_HISTORY;

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
   * @brief accept() plus the global MAC check, for an authenticated 1W frame
   *
   * Rejects the frame if either its sequence number is not fresh for this
   * source, or its MAC was already accepted from any source recently. Nothing
   * is recorded unless both checks pass. This is the overload a receive path
   * should use; the MAC-less accept() cannot catch a re-addressed replay.
   */
  bool accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence,
              const uint8_t mac[HMAC_SIZE]);

  /**
   * @brief Test a sequence number without recording it
   *
   * Useful when a frame still has to pass other checks before being accepted.
   */
  bool would_accept(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence) const;

  /**
   * @brief Check a received MAC against the recent history and record it
   *
   * For frames whose freshness does not come from a sequence number. Rejects
   * a MAC accepted from ANY node within the last GLOBAL_MAC_HISTORY accepted
   * frames - a protocol-level repeat, which should only be acted on once, or a
   * replay, possibly re-addressed. In strict mode an unknown source is
   * rejected outright.
   *
   * Spec-conformant 2W frames carry no MAC trailer at all (their freshness is
   * the 0x3C/0x3D exchange), so this is only reached by frames a caller parsed
   * with an explicit trailer. See docs/SECURITY-MODEL.md.
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
   * A primed node is also a known peer for strict mode, and its sequence is
   * treated as real: the next frame must move forward from it.
   *
   * @return false if the table is full of known peers (none is evicted)
   */
  bool prime(const uint8_t node_id[NODE_ID_SIZE], uint16_t sequence);

  /**
   * @brief Register a node as a known peer without a sequence number
   *
   * For strict mode when no persisted sequence is available: the node is
   * allowed to send, and its first frame establishes the sequence. Known peers
   * are not evicted to make room for other nodes while a non-known slot exists.
   *
   * @return false if the table is full of known peers
   */
  bool add_known_peer(const uint8_t node_id[NODE_ID_SIZE]);

  /**
   * @brief Only accept frames from nodes registered with prime()/add_known_peer()
   *
   * Off by default. With it on, accept()/accept_mac() reject - and never
   * allocate an entry for - a source this guard was not told about.
   */
  void set_strict(bool strict) { strict_ = strict; }
  bool is_strict() const { return strict_; }

  /**
   * @brief Look up the last accepted sequence number for a node
   *
   * @param node_id Source node ID (3 bytes)
   * @param sequence_out Receives the stored value when the node is known
   * @return true if the node is tracked AND has a real sequence (heard or primed)
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
    /// last_sequence holds a value actually heard or primed. accept_mac() can
    /// create an entry without one, and treating its zero as real made the
    /// first 1W frame from that node fail the window check.
    bool seq_valid;
    /// Registered through prime()/add_known_peer(): allowed in strict mode and
    /// evicted only when no other slot is left.
    bool known;
  };

  Entry* find(const uint8_t node_id[NODE_ID_SIZE]);
  const Entry* find(const uint8_t node_id[NODE_ID_SIZE]) const;
  /// Claim a slot for @p node_id: a free one, else the LRU non-known entry,
  /// else (only if @p may_evict_known) the LRU known peer. nullptr if none.
  Entry* allocate(const uint8_t node_id[NODE_ID_SIZE], bool may_evict_known);

  /// Find the node's entry; create one unless strict mode forbids it.
  Entry* lookup_for_accept(const uint8_t node_id[NODE_ID_SIZE]);
  bool sequence_ok(const Entry* entry, uint16_t sequence) const;
  bool mac_seen(const uint8_t mac[HMAC_SIZE]) const;
  void remember_mac(const uint8_t mac[HMAC_SIZE]);

  Entry entries_[MAX_NODES];

  uint8_t mac_ring_[GLOBAL_MAC_HISTORY][HMAC_SIZE];
  uint8_t mac_count_;
  uint8_t mac_next_;
  bool strict_;
  uint16_t window_;
  uint32_t tick_;
  uint32_t rejected_count_;
};

} // namespace iohome
