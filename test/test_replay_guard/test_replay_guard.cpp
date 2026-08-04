/**
 * Unit tests for the rolling-code replay guard.
 *
 * Verifying a frame's MAC proves it was produced by someone holding the system
 * key - it does not prove the frame is fresh. These tests pin down the freshness
 * rules that stop a recorded frame from being replayed off the air.
 */

#include <unity.h>
#include "protocol/iohome_replay_guard.h"

#include <string.h>

using iohome::ReplayGuard;

static const uint8_t NODE_A[3] = {0x1A, 0x38, 0x0B};
static const uint8_t NODE_B[3] = {0x70, 0x87, 0x58};

void test_first_frame_from_unknown_node_is_accepted(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 1234));
    TEST_ASSERT_EQUAL_UINT(1, guard.tracked_count());

    uint16_t stored = 0;
    TEST_ASSERT_TRUE(guard.last_sequence(NODE_A, stored));
    TEST_ASSERT_EQUAL_UINT16(1234, stored);
}

void test_exact_replay_is_rejected(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 100));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 100));
    TEST_ASSERT_EQUAL_UINT32(1, guard.rejected_count());

    // The stored counter must not move on a rejected frame.
    uint16_t stored = 0;
    guard.last_sequence(NODE_A, stored);
    TEST_ASSERT_EQUAL_UINT16(100, stored);
}

void test_old_sequence_is_rejected(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 500));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 499));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 1));
}

void test_forward_progress_is_accepted(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 10));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 11));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 12));
    TEST_ASSERT_EQUAL_UINT32(0, guard.rejected_count());
}

void test_missed_frames_within_window_are_tolerated(void) {
    // The transmitter counts every frame it sends, including ones this receiver
    // never heard. A moderate jump forward must still be accepted.
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 10));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 10 + ReplayGuard::DEFAULT_WINDOW));
}

void test_jump_beyond_window_is_rejected(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 10));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 10 + ReplayGuard::DEFAULT_WINDOW + 1));
}

void test_window_is_configurable(void) {
    ReplayGuard guard;
    guard.set_window(5);
    TEST_ASSERT_EQUAL_UINT16(5, guard.get_window());

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 100));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 105));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 111));
}

void test_zero_window_accepts_only_next_sequence(void) {
    ReplayGuard guard;
    guard.set_window(0);

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 100));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 102));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 101));
}

void test_counter_wraparound_is_handled(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 0xFFFE));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 0xFFFF));
    // 0xFFFF -> 0x0000 is a forward step of one, not a rewind.
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 0x0000));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 0x0001));

    // ...and the frame from just before the wrap is still a replay.
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 0xFFFF));
}

void test_nodes_are_tracked_independently(void) {
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 5000));
    // A low sequence number from a different node is a first sighting, not a replay.
    TEST_ASSERT_TRUE(guard.accept(NODE_B, 3));
    TEST_ASSERT_EQUAL_UINT(2, guard.tracked_count());

    TEST_ASSERT_FALSE(guard.accept(NODE_A, 5000));
    TEST_ASSERT_TRUE(guard.accept(NODE_B, 4));
}

void test_would_accept_does_not_mutate(void) {
    ReplayGuard guard;
    guard.accept(NODE_A, 10);

    TEST_ASSERT_TRUE(guard.would_accept(NODE_A, 11));
    TEST_ASSERT_TRUE(guard.would_accept(NODE_A, 11));  // still true - nothing recorded
    TEST_ASSERT_FALSE(guard.would_accept(NODE_A, 10));

    TEST_ASSERT_TRUE(guard.accept(NODE_A, 11));
    TEST_ASSERT_FALSE(guard.would_accept(NODE_A, 11));
}

void test_prime_seeds_a_node(void) {
    // After a reboot the receiver restores the persisted counter so an attacker
    // cannot replay anything recorded before the restart.
    ReplayGuard guard;

    TEST_ASSERT_TRUE(guard.prime(NODE_A, 9000));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 8999));
    TEST_ASSERT_FALSE(guard.accept(NODE_A, 9000));
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 9001));
}

void test_forget_and_reset(void) {
    ReplayGuard guard;
    guard.accept(NODE_A, 10);
    guard.accept(NODE_B, 20);

    TEST_ASSERT_TRUE(guard.forget(NODE_A));
    TEST_ASSERT_FALSE(guard.forget(NODE_A));
    TEST_ASSERT_EQUAL_UINT(1, guard.tracked_count());

    // Forgotten node starts fresh again.
    TEST_ASSERT_TRUE(guard.accept(NODE_A, 5));

    guard.reset();
    TEST_ASSERT_EQUAL_UINT(0, guard.tracked_count());
    TEST_ASSERT_EQUAL_UINT32(0, guard.rejected_count());
}

void test_table_evicts_least_recently_used(void) {
    ReplayGuard guard;

    // Fill the table.
    for (size_t i = 0; i < ReplayGuard::MAX_NODES; i++) {
        const uint8_t node[3] = {0x00, 0x00, static_cast<uint8_t>(i)};
        TEST_ASSERT_TRUE(guard.accept(node, 100));
    }
    TEST_ASSERT_EQUAL_UINT(ReplayGuard::MAX_NODES, guard.tracked_count());

    // Touch node 0 so it is no longer the least recently used.
    const uint8_t node0[3] = {0x00, 0x00, 0x00};
    TEST_ASSERT_TRUE(guard.accept(node0, 101));

    // One more node forces an eviction; node 0 must survive it.
    const uint8_t newcomer[3] = {0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(guard.accept(newcomer, 1));
    TEST_ASSERT_EQUAL_UINT(ReplayGuard::MAX_NODES, guard.tracked_count());

    uint16_t stored = 0;
    TEST_ASSERT_TRUE(guard.last_sequence(node0, stored));
    TEST_ASSERT_EQUAL_UINT16(101, stored);
}

void test_rejects_nullptr(void) {
    ReplayGuard guard;

    TEST_ASSERT_FALSE(guard.accept(nullptr, 1));
    TEST_ASSERT_FALSE(guard.would_accept(nullptr, 1));
    TEST_ASSERT_FALSE(guard.prime(nullptr, 1));
    TEST_ASSERT_FALSE(guard.forget(nullptr));

    uint16_t stored = 0;
    TEST_ASSERT_FALSE(guard.last_sequence(nullptr, stored));
    TEST_ASSERT_FALSE(guard.last_sequence(NODE_A, stored));
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_first_frame_from_unknown_node_is_accepted);
    RUN_TEST(test_exact_replay_is_rejected);
    RUN_TEST(test_old_sequence_is_rejected);
    RUN_TEST(test_forward_progress_is_accepted);
    RUN_TEST(test_missed_frames_within_window_are_tolerated);
    RUN_TEST(test_jump_beyond_window_is_rejected);
    RUN_TEST(test_window_is_configurable);
    RUN_TEST(test_zero_window_accepts_only_next_sequence);
    RUN_TEST(test_counter_wraparound_is_handled);
    RUN_TEST(test_nodes_are_tracked_independently);
    RUN_TEST(test_would_accept_does_not_mutate);
    RUN_TEST(test_prime_seeds_a_node);
    RUN_TEST(test_forget_and_reset);
    RUN_TEST(test_table_evicts_least_recently_used);
    RUN_TEST(test_rejects_nullptr);

    return UNITY_END();
}
