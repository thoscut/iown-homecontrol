#include <unity.h>
#include "protocol/iohome_2w.h"
#include "protocol/iohome_constants.h"
#include <string.h>

void test_channel_hopper_initial_state(void) {
    iohome::mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
    TEST_ASSERT_FALSE(hopper.is_enabled());
}

void test_channel_hopper_frequency(void) {
    iohome::mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 868.95f, hopper.get_current_frequency());
}

void test_channel_hopper_disabled_no_hop(void) {
    iohome::mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_FALSE(hopper.update(10000));
    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
}

void test_channel_hopper_enabled_hops(void) {
    iohome::mode2w::ChannelHopper hopper;
    hopper.begin(2.7f);
    hopper.set_enabled(true);

    TEST_ASSERT_FALSE(hopper.update(0));

    TEST_ASSERT_TRUE(hopper.update(3));
    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_3, hopper.get_current_channel());
}

void test_channel_hopper_cycles(void) {
    iohome::mode2w::ChannelHopper hopper;
    hopper.begin(1.0f);
    hopper.set_enabled(true);

    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());

    hopper.update(2);
    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_3, hopper.get_current_channel());

    hopper.update(4);
    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_1, hopper.get_current_channel());

    hopper.update(6);
    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
}

void test_channel_hopper_reset(void) {
    iohome::mode2w::ChannelHopper hopper;
    hopper.begin(1.0f);
    hopper.set_enabled(true);

    hopper.update(2);
    TEST_ASSERT_NOT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());

    hopper.reset();
    TEST_ASSERT_EQUAL(iohome::mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
}

void test_discovery_manager_initial(void) {
    iohome::mode2w::DiscoveryManager mgr;
    uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);

    TEST_ASSERT_EQUAL(0, mgr.get_discovered_count());
}

void test_discovery_manager_create_request(void) {
    iohome::mode2w::DiscoveryManager mgr;
    uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);

    iohome::frame::IoFrame frame;
    TEST_ASSERT_TRUE(mgr.create_discovery_request(&frame, 0xFF));

    TEST_ASSERT_EQUAL_UINT8(0x00, frame.dest_node[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.dest_node[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.dest_node[2]);

    TEST_ASSERT_EQUAL_UINT8(0x11, frame.src_node[0]);
}

void test_beacon_handler_no_beacon(void) {
    iohome::mode2w::BeaconHandler handler;
    handler.begin();

    TEST_ASSERT_FALSE(handler.has_recent_beacon());

    iohome::mode2w::BeaconInfo info;
    TEST_ASSERT_FALSE(handler.get_last_beacon(&info));
}

void test_auth_manager_initial_state(void) {
    iohome::mode2w::AuthenticationManager auth;
    uint8_t key[16] = {0};
    auth.begin(key);

    TEST_ASSERT_EQUAL(iohome::mode2w::ChallengeState::IDLE, auth.get_state());
}

void test_auth_manager_generate_challenge(void) {
    iohome::mode2w::AuthenticationManager auth;
    uint8_t key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    auth.begin(key);

    uint8_t challenge[6];
    auth.generate_challenge(challenge);

    TEST_ASSERT_EQUAL(iohome::mode2w::ChallengeState::CHALLENGE_SENT, auth.get_state());

    const uint8_t* stored = auth.get_current_challenge();
    TEST_ASSERT_EQUAL_UINT8_ARRAY(challenge, stored, 6);
}

void test_auth_manager_reset(void) {
    iohome::mode2w::AuthenticationManager auth;
    uint8_t key[16] = {0};
    auth.begin(key);

    uint8_t challenge[6];
    auth.generate_challenge(challenge);
    TEST_ASSERT_EQUAL(iohome::mode2w::ChallengeState::CHALLENGE_SENT, auth.get_state());

    auth.reset();
    TEST_ASSERT_EQUAL(iohome::mode2w::ChallengeState::IDLE, auth.get_state());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_channel_hopper_initial_state);
    RUN_TEST(test_channel_hopper_frequency);
    RUN_TEST(test_channel_hopper_disabled_no_hop);
    RUN_TEST(test_channel_hopper_enabled_hops);
    RUN_TEST(test_channel_hopper_cycles);
    RUN_TEST(test_channel_hopper_reset);
    RUN_TEST(test_discovery_manager_initial);
    RUN_TEST(test_discovery_manager_create_request);
    RUN_TEST(test_beacon_handler_no_beacon);
    RUN_TEST(test_auth_manager_initial_state);
    RUN_TEST(test_auth_manager_generate_challenge);
    RUN_TEST(test_auth_manager_reset);

    return UNITY_END();
}
