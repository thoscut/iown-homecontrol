/**
 * Unit tests for the io-homecontrol 2-Way mode helpers.
 */

#include <unity.h>
#include "protocol/iohome_2w.h"
#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"
#include <string.h>

namespace mode2w = iohome::mode2w;

// ---------------------------------------------------------------------------
// Frequency hopping
// ---------------------------------------------------------------------------

void test_channel_hopper_initial_state(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
    TEST_ASSERT_FALSE(hopper.is_enabled());
}

void test_channel_hopper_frequency(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 868.95f, hopper.get_current_frequency());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 868.25f,
                             mode2w::ChannelHopper::frequency_of(mode2w::ChannelState::CHANNEL_1));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 869.85f,
                             mode2w::ChannelHopper::frequency_of(mode2w::ChannelState::CHANNEL_3));
}

void test_channel_hopper_default_interval_is_microsecond_accurate(void) {
    // The protocol dwell time is 2.7 ms - it cannot be expressed at
    // millisecond resolution, so the hopper must keep microseconds internally.
    mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_EQUAL_UINT32(2700, hopper.get_hop_interval_us());
}

void test_channel_hopper_disabled_no_hop(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin();

    TEST_ASSERT_FALSE(hopper.update_us(10000000));
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
}

void test_channel_hopper_hops_on_interval(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin(2.7f);
    hopper.set_enabled(true);
    hopper.reset(0);

    TEST_ASSERT_FALSE(hopper.update_us(2699));
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());

    TEST_ASSERT_TRUE(hopper.update_us(2700));
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_3, hopper.get_current_channel());
}

void test_channel_hopper_cycles(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin(1.0f);
    hopper.set_enabled(true);
    hopper.reset(0);

    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());

    hopper.update_us(1000);
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_3, hopper.get_current_channel());

    hopper.update_us(2000);
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_1, hopper.get_current_channel());

    hopper.update_us(3000);
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
}

void test_channel_hopper_survives_counter_wrap(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin(1.0f);
    hopper.set_enabled(true);

    // Start just before the 32-bit microsecond counter wraps (~71.6 minutes).
    const unsigned long near_max = 0xFFFFFF00UL;
    hopper.reset(near_max);

    TEST_ASSERT_FALSE(hopper.update_us(near_max + 500));
    TEST_ASSERT_TRUE(hopper.update_us(near_max + 1000));  // wraps to 0x000000FC
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_3, hopper.get_current_channel());
}

void test_channel_hopper_time_until_next_hop(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin(2.7f);
    hopper.reset(1000);

    TEST_ASSERT_EQUAL_UINT32(2700, hopper.time_until_next_hop_us(1000));
    TEST_ASSERT_EQUAL_UINT32(700, hopper.time_until_next_hop_us(3000));
    TEST_ASSERT_EQUAL_UINT32(0, hopper.time_until_next_hop_us(4000));
}

void test_channel_hopper_reset(void) {
    mode2w::ChannelHopper hopper;
    hopper.begin(1.0f);
    hopper.set_enabled(true);
    hopper.reset(0);

    hopper.update_us(1000);
    TEST_ASSERT_NOT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());

    hopper.reset(0);
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_2, hopper.get_current_channel());
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

void test_discovery_manager_initial(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    TEST_ASSERT_TRUE(mgr.begin(node));
    TEST_ASSERT_FALSE(mgr.begin(nullptr));

    TEST_ASSERT_EQUAL_UINT(0, mgr.get_discovered_count());
    TEST_ASSERT_EQUAL(mode2w::DiscoveryState::IDLE, mgr.get_state());
}

void test_discovery_request_is_broadcast_and_finalized(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);

    iohome::frame::IoFrame frame;
    TEST_ASSERT_TRUE(mgr.create_discovery_request(&frame, 0xFF));

    // docs/linklayer.md: discovery is a plain 2W broadcast using command 0x28.
    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_FALSE(frame.authenticated);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_DISCOVER, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(iohome::ADDRESS_BROADCAST, frame.dest_node, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(node, frame.src_node, 3);

    // A frame that was never finalized would carry a zero CRC and be dropped
    // by every receiver.
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&frame));
    TEST_ASSERT_FALSE(frame.crc[0] == 0x00 && frame.crc[1] == 0x00);
}

void test_discovery_collects_multiple_devices(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);
    mgr.start_discovery(0xFF, 10000, 0);

    iohome::frame::IoFrame answer;
    iohome::frame::init_frame(&answer, false);

    const uint8_t payload[] = {0x03, 0x01, 0x00};

    for (uint8_t i = 1; i <= 3; i++) {
        const uint8_t src[3] = {0xAA, 0xBB, i};
        iohome::frame::set_source(&answer, src);
        TEST_ASSERT_TRUE(iohome::frame::set_command(&answer, iohome::CMD_DISCOVER_ANSWER,
                                                    payload, sizeof(payload)));
        TEST_ASSERT_TRUE(mgr.process_discovery_response(&answer, -70, 100));
    }

    // Regression: the manager used to leave DISCOVERING after the first answer,
    // so every later device was silently dropped.
    TEST_ASSERT_EQUAL_UINT(3, mgr.get_discovered_count());

    mode2w::DiscoveredDevice device;
    TEST_ASSERT_TRUE(mgr.get_discovered_device(2, &device));
    TEST_ASSERT_EQUAL_UINT8(0x03, device.node_id[2]);
    TEST_ASSERT_EQUAL(iohome::DeviceType::WINDOW_OPENER, device.device_type);
    TEST_ASSERT_EQUAL_UINT8(0x01, device.manufacturer);

    TEST_ASSERT_FALSE(mgr.get_discovered_device(3, &device));
    TEST_ASSERT_FALSE(mgr.get_discovered_device(0, nullptr));
}

void test_discovery_ignores_duplicates(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);
    mgr.start_discovery(0xFF, 10000, 0);

    iohome::frame::IoFrame answer;
    iohome::frame::init_frame(&answer, false);
    const uint8_t src[3] = {0xAA, 0xBB, 0xCC};
    iohome::frame::set_source(&answer, src);
    const uint8_t payload[] = {0x00, 0x02};
    iohome::frame::set_command(&answer, iohome::CMD_DISCOVER_ANSWER, payload, sizeof(payload));

    TEST_ASSERT_TRUE(mgr.process_discovery_response(&answer, -70, 100));
    TEST_ASSERT_FALSE(mgr.process_discovery_response(&answer, -70, 200));
    TEST_ASSERT_EQUAL_UINT(1, mgr.get_discovered_count());
}

void test_discovery_ignores_unrelated_commands(void) {
    // Regression: every received frame used to be fed to the discovery manager,
    // so ordinary traffic ended up in the discovered-device list.
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);
    mgr.start_discovery(0xFF, 10000, 0);

    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, false);
    const uint8_t src[3] = {0xDE, 0xAD, 0xBE};
    iohome::frame::set_source(&frame, src);
    iohome::frame::set_execute_command(&frame, iohome::MP_CLOSE);

    TEST_ASSERT_FALSE(mgr.process_discovery_response(&frame, -70, 100));
    TEST_ASSERT_EQUAL_UINT(0, mgr.get_discovered_count());
}

void test_discovery_times_out(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);
    mgr.start_discovery(0xFF, 1000, 5000);

    TEST_ASSERT_TRUE(mgr.update(5500));
    TEST_ASSERT_EQUAL(mode2w::DiscoveryState::DISCOVERING, mgr.get_state());

    TEST_ASSERT_FALSE(mgr.update(6000));
    TEST_ASSERT_EQUAL(mode2w::DiscoveryState::TIMED_OUT, mgr.get_state());

    // Answers arriving after the window closes are ignored.
    iohome::frame::IoFrame answer;
    iohome::frame::init_frame(&answer, false);
    iohome::frame::set_command(&answer, iohome::CMD_DISCOVER_ANSWER, nullptr, 0);
    TEST_ASSERT_FALSE(mgr.process_discovery_response(&answer, -70, 7000));
}

void test_key_transfer_frames_are_valid(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);

    const uint8_t dest[3] = {0xAA, 0xBB, 0xCC};
    const uint8_t key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                             0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    // Regression: these frames used to exceed the (wrongly sized) maximum frame
    // and could never be built, so pairing simply never worked.
    iohome::frame::IoFrame frame;
    TEST_ASSERT_TRUE(mgr.create_key_transfer_1w(&frame, dest, node, key, 0x02, 0x0C25));
    TEST_ASSERT_TRUE(frame.is_1w_mode);
    TEST_ASSERT_FALSE(frame.authenticated);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_SEND_1W_KEY, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(20, frame.data_len);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&frame));

    // The transported key must round-trip through the documented masking.
    uint8_t recovered[16];
    TEST_ASSERT_TRUE(iohome::crypto::decrypt_1w_key(frame.data, dest, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(key, recovered, 16);

    const uint8_t challenge[6] = {1, 2, 3, 4, 5, 6};
    TEST_ASSERT_TRUE(mgr.create_key_transfer_2w(&frame, dest, node, key, challenge));
    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_KEY_TRANSFER, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(16, frame.data_len);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&frame));

    TEST_ASSERT_TRUE(iohome::crypto::decrypt_2w_key(frame.data, challenge, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(key, recovered, 16);
}

void test_remove_1w_controller_frame(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    mgr.begin(node);

    const uint8_t dest[3] = {0xAA, 0xBB, 0xCC};
    iohome::frame::IoFrame frame;
    TEST_ASSERT_TRUE(mgr.create_remove_1w_controller(&frame, dest, node));

    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_REMOVE_1W_CONTROLLER, frame.command_id);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&frame));
}

void test_discovery_rejects_nullptr(void) {
    mode2w::DiscoveryManager mgr;
    const uint8_t node[3] = {0x11, 0x22, 0x33};
    const uint8_t key[16] = {0};
    const uint8_t challenge[6] = {0};
    mgr.begin(node);

    TEST_ASSERT_FALSE(mgr.create_discovery_request(nullptr, 0xFF));
    TEST_ASSERT_FALSE(mgr.process_discovery_response(nullptr, 0, 0));

    iohome::frame::IoFrame frame;
    TEST_ASSERT_FALSE(mgr.create_key_transfer_1w(nullptr, node, node, key));
    TEST_ASSERT_FALSE(mgr.create_key_transfer_1w(&frame, nullptr, node, key));
    TEST_ASSERT_FALSE(mgr.create_key_transfer_1w(&frame, node, node, nullptr));
    TEST_ASSERT_FALSE(mgr.create_key_transfer_2w(&frame, node, node, key, nullptr));
    TEST_ASSERT_FALSE(mgr.create_key_transfer_2w(&frame, node, node, nullptr, challenge));
}

// ---------------------------------------------------------------------------
// Beacons
// ---------------------------------------------------------------------------

void test_beacon_handler_no_beacon(void) {
    mode2w::BeaconHandler handler;
    handler.begin();

    TEST_ASSERT_FALSE(handler.has_recent_beacon(1000));

    mode2w::BeaconInfo info;
    TEST_ASSERT_FALSE(handler.get_last_beacon(&info));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFF, handler.time_since_last_beacon(1000));
}

void test_beacon_handler_records_beacon(void) {
    mode2w::BeaconHandler handler;
    handler.begin();

    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, false);
    frame.ctrl_byte_1 |= iohome::CTRL1_USE_BEACON;
    const uint8_t src[3] = {0x01, 0x02, 0x03};
    iohome::frame::set_source(&frame, src);
    const uint8_t payload[] = {0x02, 0xAB};
    iohome::frame::set_command(&frame, 0x00, payload, sizeof(payload));

    TEST_ASSERT_TRUE(handler.process_beacon(&frame, -80, 7.5f, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, handler.beacon_count());

    mode2w::BeaconInfo info;
    TEST_ASSERT_TRUE(handler.get_last_beacon(&info));
    TEST_ASSERT_EQUAL(mode2w::BeaconType::SYSTEM_BEACON, info.type);
    TEST_ASSERT_EQUAL_UINT8(2, info.data_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, info.node_id, 3);
    TEST_ASSERT_EQUAL_INT(-80, info.rssi);

    TEST_ASSERT_TRUE(handler.has_recent_beacon(2000, 5000));
    TEST_ASSERT_FALSE(handler.has_recent_beacon(9000, 5000));
    TEST_ASSERT_EQUAL_UINT32(1000, handler.time_since_last_beacon(2000));
}

void test_beacon_handler_ignores_non_beacon(void) {
    mode2w::BeaconHandler handler;
    handler.begin();

    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, false);  // beacon flag not set

    TEST_ASSERT_FALSE(handler.process_beacon(&frame, -80, 1.0f, 1000));
    TEST_ASSERT_FALSE(handler.process_beacon(nullptr, -80, 1.0f, 1000));

    mode2w::BeaconInfo info;
    TEST_ASSERT_FALSE(handler.get_last_beacon(&info));
    TEST_ASSERT_FALSE(handler.get_last_beacon(nullptr));
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

void test_auth_manager_initial_state(void) {
    mode2w::AuthenticationManager auth;
    const uint8_t key[16] = {0};

    TEST_ASSERT_FALSE(auth.begin(nullptr));
    TEST_ASSERT_TRUE(auth.begin(key));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, auth.get_state(0));
}

void test_auth_manager_generates_unpredictable_challenges(void) {
    mode2w::AuthenticationManager auth;
    const uint8_t key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                             0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    auth.begin(key);

    uint8_t first[6], second[6];
    TEST_ASSERT_TRUE(auth.generate_challenge(first));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::CHALLENGE_SENT, auth.peek_state());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, auth.get_current_challenge(), 6);

    TEST_ASSERT_TRUE(auth.generate_challenge(second));

    // Regression: challenges used to come from an unseeded Arduino PRNG, so
    // every device produced the same sequence after every reboot.
    TEST_ASSERT_FALSE(memcmp(first, second, 6) == 0);

    TEST_ASSERT_FALSE(auth.generate_challenge(nullptr));
}

void test_auth_manager_reset(void) {
    mode2w::AuthenticationManager auth;
    const uint8_t key[16] = {0};
    auth.begin(key);

    uint8_t challenge[6];
    TEST_ASSERT_TRUE(auth.generate_challenge(challenge));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::CHALLENGE_SENT, auth.peek_state());

    auth.reset();
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, auth.peek_state());
}

void test_auth_handshake_succeeds(void) {
    const uint8_t key[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                             0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager responder;
    initiator.begin(key);
    responder.begin(key);

    iohome::frame::IoFrame request;
    TEST_ASSERT_TRUE(initiator.create_challenge_request(&request, actuator, controller));
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_CHALLENGE_REQUEST, request.command_id);
    TEST_ASSERT_TRUE(request.authenticated);

    // The responder echoes the challenge it received.
    iohome::frame::IoFrame response;
    TEST_ASSERT_TRUE(responder.create_challenge_response(&response, controller, actuator,
                                                         request.data));

    TEST_ASSERT_TRUE(initiator.verify_challenge_response(&response, 100));
    TEST_ASSERT_TRUE(initiator.is_authenticated(100));
}

void test_auth_rejects_replayed_response(void) {
    const uint8_t key[16] = {0x42};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager responder;
    initiator.begin(key);
    responder.begin(key);

    iohome::frame::IoFrame request;
    TEST_ASSERT_TRUE(initiator.create_challenge_request(&request, actuator, controller));

    iohome::frame::IoFrame response;
    TEST_ASSERT_TRUE(responder.create_challenge_response(&response, controller, actuator,
                                                         request.data));

    TEST_ASSERT_TRUE(initiator.verify_challenge_response(&response, 100));

    // The challenge is single-use: replaying the very same response must fail.
    TEST_ASSERT_FALSE(initiator.verify_challenge_response(&response, 150));
}

void test_challenge_stays_usable_after_handshake(void) {
    // Regression: the challenge used to be wiped on a successful handshake, so
    // the very commands the handshake authorises could no longer be MAC'd.
    const uint8_t key[16] = {0x42};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager responder;
    initiator.begin(key);
    responder.begin(key);

    TEST_ASSERT_FALSE(initiator.has_active_challenge(0));

    iohome::frame::IoFrame request;
    TEST_ASSERT_TRUE(initiator.create_challenge_request(&request, actuator, controller));
    TEST_ASSERT_TRUE(initiator.has_active_challenge(0));

    uint8_t nonce[6];
    memcpy(nonce, initiator.get_current_challenge(), 6);

    iohome::frame::IoFrame response;
    TEST_ASSERT_TRUE(responder.create_challenge_response(&response, controller, actuator,
                                                         request.data));
    TEST_ASSERT_TRUE(initiator.verify_challenge_response(&response, 100));

    TEST_ASSERT_TRUE(initiator.has_active_challenge(100));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(nonce, initiator.get_current_challenge(), 6);

    // ...and it goes away with the session.
    TEST_ASSERT_FALSE(initiator.has_active_challenge(1000000));
}

void test_failed_handshake_clears_challenge(void) {
    const uint8_t key[16] = {0x42};
    const uint8_t other_key[16] = {0x24};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager impostor;
    initiator.begin(key);
    impostor.begin(other_key);

    iohome::frame::IoFrame request;
    initiator.create_challenge_request(&request, actuator, controller);

    iohome::frame::IoFrame response;
    impostor.create_challenge_response(&response, controller, actuator, request.data);

    TEST_ASSERT_FALSE(initiator.verify_challenge_response(&response, 100));

    // A bad answer burns the nonce, so an attacker cannot keep guessing
    // against one known challenge.
    TEST_ASSERT_FALSE(initiator.has_active_challenge(100));

    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, initiator.get_current_challenge(), 6);
}

void test_auth_rejects_wrong_key(void) {
    const uint8_t key[16] = {0x42};
    const uint8_t other_key[16] = {0x24};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager impostor;
    initiator.begin(key);
    impostor.begin(other_key);

    iohome::frame::IoFrame request;
    TEST_ASSERT_TRUE(initiator.create_challenge_request(&request, actuator, controller));

    iohome::frame::IoFrame response;
    TEST_ASSERT_TRUE(impostor.create_challenge_response(&response, controller, actuator,
                                                        request.data));

    TEST_ASSERT_FALSE(initiator.verify_challenge_response(&response, 100));
    TEST_ASSERT_FALSE(initiator.is_authenticated(100));
}

void test_auth_challenge_times_out(void) {
    const uint8_t key[16] = {0x42};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager responder;
    initiator.begin(key);
    responder.begin(key);
    initiator.set_challenge_timeout_ms(5000);

    iohome::frame::IoFrame request;
    TEST_ASSERT_TRUE(initiator.create_challenge_request(&request, actuator, controller));

    iohome::frame::IoFrame response;
    TEST_ASSERT_TRUE(responder.create_challenge_response(&response, controller, actuator,
                                                         request.data));

    // Answer arrives after the challenge expired.
    TEST_ASSERT_FALSE(initiator.verify_challenge_response(&response, 6000));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, initiator.peek_state());
}

void test_auth_session_expires(void) {
    const uint8_t key[16] = {0x42};
    const uint8_t controller[3] = {0x01, 0x02, 0x03};
    const uint8_t actuator[3] = {0x0A, 0x0B, 0x0C};

    mode2w::AuthenticationManager initiator;
    mode2w::AuthenticationManager responder;
    initiator.begin(key);
    responder.begin(key);
    initiator.set_session_timeout_ms(30000);

    iohome::frame::IoFrame request;
    initiator.create_challenge_request(&request, actuator, controller);
    iohome::frame::IoFrame response;
    responder.create_challenge_response(&response, controller, actuator, request.data);

    TEST_ASSERT_TRUE(initiator.verify_challenge_response(&response, 1000));
    TEST_ASSERT_TRUE(initiator.is_authenticated(20000));

    // Regression: an authenticated session used to stay valid forever.
    TEST_ASSERT_FALSE(initiator.is_authenticated(40000));
}

void test_auth_rejects_nullptr(void) {
    mode2w::AuthenticationManager auth;
    const uint8_t key[16] = {0};
    const uint8_t node[3] = {1, 2, 3};
    auth.begin(key);

    iohome::frame::IoFrame frame;
    TEST_ASSERT_FALSE(auth.create_challenge_request(nullptr, node, node));
    TEST_ASSERT_FALSE(auth.create_challenge_request(&frame, nullptr, node));
    TEST_ASSERT_FALSE(auth.create_challenge_response(&frame, node, node, nullptr));
    TEST_ASSERT_FALSE(auth.verify_challenge_response(nullptr, 0));
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_channel_hopper_initial_state);
    RUN_TEST(test_channel_hopper_frequency);
    RUN_TEST(test_channel_hopper_default_interval_is_microsecond_accurate);
    RUN_TEST(test_channel_hopper_disabled_no_hop);
    RUN_TEST(test_channel_hopper_hops_on_interval);
    RUN_TEST(test_channel_hopper_cycles);
    RUN_TEST(test_channel_hopper_survives_counter_wrap);
    RUN_TEST(test_channel_hopper_time_until_next_hop);
    RUN_TEST(test_channel_hopper_reset);

    RUN_TEST(test_discovery_manager_initial);
    RUN_TEST(test_discovery_request_is_broadcast_and_finalized);
    RUN_TEST(test_discovery_collects_multiple_devices);
    RUN_TEST(test_discovery_ignores_duplicates);
    RUN_TEST(test_discovery_ignores_unrelated_commands);
    RUN_TEST(test_discovery_times_out);
    RUN_TEST(test_key_transfer_frames_are_valid);
    RUN_TEST(test_remove_1w_controller_frame);
    RUN_TEST(test_discovery_rejects_nullptr);

    RUN_TEST(test_beacon_handler_no_beacon);
    RUN_TEST(test_beacon_handler_records_beacon);
    RUN_TEST(test_beacon_handler_ignores_non_beacon);

    RUN_TEST(test_auth_manager_initial_state);
    RUN_TEST(test_auth_manager_generates_unpredictable_challenges);
    RUN_TEST(test_auth_manager_reset);
    RUN_TEST(test_auth_handshake_succeeds);
    RUN_TEST(test_auth_rejects_replayed_response);
    RUN_TEST(test_challenge_stays_usable_after_handshake);
    RUN_TEST(test_failed_handshake_clears_challenge);
    RUN_TEST(test_auth_rejects_wrong_key);
    RUN_TEST(test_auth_challenge_times_out);
    RUN_TEST(test_auth_session_expires);
    RUN_TEST(test_auth_rejects_nullptr);

    return UNITY_END();
}
