/**
 * Unit tests for the io-homecontrol 2-Way mode helpers.
 */

#include <unity.h>
#include "protocol/iohome_2w.h"
#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"
#include <stdio.h>
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
    hopper.begin(1.0f);  // 1000 us hop interval
    hopper.set_enabled(true);

    // Start just before the 32-bit microsecond counter wraps (~71.6 minutes).
    // The post-wrap timestamps are passed as genuinely-reduced uint32_t values,
    // so `now` is numerically *smaller* than last_hop and the wrap-safe
    // subtraction is actually exercised. The old form used near_max + offset,
    // which on a 64-bit host never wraps at 2^32 - so its "wrap" was fiction and
    // a regression in the subtraction would not have been caught. The hopper now
    // does its timestamp math in uint32_t, so this crosses the real 2^32 boundary
    // on host and target alike.
    const uint32_t near_max = 0xFFFFFF00u;
    hopper.reset(near_max);

    // now = (near_max + 0x134) mod 2^32 = 0x34; elapsed = 0x34 - 0xFFFFFF00 = 308 us (< 1000): no hop.
    TEST_ASSERT_FALSE(hopper.update_us(static_cast<uint32_t>(near_max + 0x134)));
    // now = 0x300; elapsed since the reset point = 0x400 = 1024 us (>= 1000): one hop.
    TEST_ASSERT_TRUE(hopper.update_us(static_cast<uint32_t>(near_max + 0x400)));
    TEST_ASSERT_EQUAL(mode2w::ChannelState::CHANNEL_3, hopper.get_current_channel());

    // time_until_next_hop_us() uses the same wrap-safe math: reset at 0xFFFFFFC0,
    // query 400 us later (which wraps past 2^32), and 600 of the 1000 us remain.
    hopper.reset(0xFFFFFFC0u);
    TEST_ASSERT_EQUAL_UINT32(600, hopper.time_until_next_hop_us(
        static_cast<uint32_t>(0xFFFFFFC0u + 400)));
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

    // A real Discover Answer, byte for byte as docs/commands.md lays it out:
    //   node type/sub-type (2) | node address (3) | OEM (1) | multi info (1)
    //   | timestamp (2)
    // 0x0101 is a window opener with an integrated rain sensor: type 4,
    // sub-type 1. Reading the type as one byte gives 0x01, an interior
    // venetian blind's high byte, and reads the OEM out of the type's low half.
    // The OEM byte is deliberately not 0x01: with Velux there, reading it from
    // the type's low half gives the right answer by accident and the test
    // proves nothing. 0x0C is Atlantic, and it matches the worked example in
    // docs/commands.md.
    const uint8_t payload[] = {0x01, 0x01, 0xAA, 0xBB, 0x03, 0x0C, 0xCC, 0x0F, 0xB8};

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

    TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(iohome::NodeType::WINDOW_OPENER_RAIN_SENSOR),
                            device.node_type);
    TEST_ASSERT_EQUAL_UINT16(4, device.type);
    TEST_ASSERT_EQUAL_UINT8(1, device.subtype);

    // The OEM byte is at offset 5, past the node address - not at offset 1.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(iohome::Manufacturer::ATLANTIC),
                            device.manufacturer);
    TEST_ASSERT_EQUAL_HEX8(0xCC, device.multi_info);
    TEST_ASSERT_EQUAL_HEX16(0x0FB8, device.device_timestamp);

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

    // The key is masked with the *source* (the key owner), so the receiver
    // unmasks with the frame's source address - not the destination.
    uint8_t recovered[16];
    TEST_ASSERT_TRUE(iohome::crypto::decrypt_1w_key(frame.data, node, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(key, recovered, 16);
    // Masking with the destination is the bug this pins: it would not round-trip
    // for a real receiver, which only knows the source.
    uint8_t wrong_addr[16];
    TEST_ASSERT_TRUE(iohome::crypto::decrypt_1w_key(frame.data, dest, wrong_addr));
    TEST_ASSERT_FALSE(memcmp(key, wrong_addr, 16) == 0);

    // The key mask depends on the frame that requested the transfer, so the
    // same frame has to be handed to both directions.
    const uint8_t challenge[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t request[1] = {iohome::CMD_ASK_CHALLENGE};
    TEST_ASSERT_TRUE(mgr.create_key_transfer_2w(&frame, dest, node, key, challenge,
                                                request, sizeof(request)));
    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_KEY_TRANSFER, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(16, frame.data_len);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&frame));

    TEST_ASSERT_TRUE(iohome::crypto::decrypt_2w_key(frame.data, request, sizeof(request),
                                                    challenge, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(key, recovered, 16);

    // A different requesting frame must produce a different mask - otherwise
    // the frame is not actually bound to the exchange that asked for it.
    const uint8_t other_request[7] = {iohome::CMD_LAUNCH_KEY_TRANSFER, 1, 2, 3, 4, 5, 6};
    uint8_t wrong[16];
    TEST_ASSERT_TRUE(iohome::crypto::decrypt_2w_key(frame.data, other_request,
                                                    sizeof(other_request), challenge, wrong));
    TEST_ASSERT_FALSE(memcmp(key, wrong, 16) == 0);
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
    const uint8_t request[1] = {iohome::CMD_ASK_CHALLENGE};
    TEST_ASSERT_FALSE(mgr.create_key_transfer_2w(&frame, node, node, key, nullptr,
                                                 request, sizeof(request)));
    TEST_ASSERT_FALSE(mgr.create_key_transfer_2w(&frame, node, node, nullptr, challenge,
                                                 request, sizeof(request)));
    // Without the requesting frame the mask cannot be derived at all.
    TEST_ASSERT_FALSE(mgr.create_key_transfer_2w(&frame, node, node, key, challenge,
                                                 nullptr, 1));
    TEST_ASSERT_FALSE(mgr.create_key_transfer_2w(&frame, node, node, key, challenge,
                                                 request, 0));
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
    TEST_ASSERT_TRUE(auth.generate_challenge(first, 0));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::CHALLENGE_SENT, auth.peek_state());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, auth.get_current_challenge(), 6);

    TEST_ASSERT_TRUE(auth.generate_challenge(second, 0));

    // Regression: challenges used to come from an unseeded Arduino PRNG, so
    // every device produced the same sequence after every reboot.
    TEST_ASSERT_FALSE(memcmp(first, second, 6) == 0);

    TEST_ASSERT_FALSE(auth.generate_challenge(nullptr, 0));
}

void test_auth_manager_reset(void) {
    mode2w::AuthenticationManager auth;
    const uint8_t key[16] = {0};
    auth.begin(key);

    uint8_t challenge[6];
    TEST_ASSERT_TRUE(auth.generate_challenge(challenge, 0));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::CHALLENGE_SENT, auth.peek_state());

    auth.reset();
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, auth.peek_state());
}

namespace {

const uint8_t KEY[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                         0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
const uint8_t CONTROLLER[3] = {0x01, 0x02, 0x03};
const uint8_t ACTUATOR[3] = {0x0A, 0x0B, 0x0C};

/// A plain 2W command from `src` to `dest`, the way a controller sends one.
iohome::frame::IoFrame make_command(const uint8_t dest[3], const uint8_t src[3],
                                    uint16_t main_param = iohome::MP_CLOSE) {
    iohome::frame::IoFrame f;
    iohome::frame::init_frame(&f, false);
    iohome::frame::set_destination(&f, dest);
    iohome::frame::set_source(&f, src);
    iohome::frame::set_execute_command(&f, main_param);
    iohome::frame::finalize_frame_plain(&f);
    return f;
}

/// Serialize and parse back, as the radio would deliver it.
iohome::frame::IoFrame over_the_air(const iohome::frame::IoFrame& f) {
    uint8_t buf[iohome::FRAME_MAX_SIZE];
    const size_t n = iohome::frame::serialize_frame(&f, buf, sizeof(buf));
    iohome::frame::IoFrame out;
    if (!iohome::frame::parse_frame(buf, n, &out)) {
        memset(&out, 0, sizeof(out));  // fails every assertion that follows
    }
    return out;
}

size_t unhex(const char* s, uint8_t* out) {
    size_t n = 0;
    while (s[0] != '\0' && s[1] != '\0') {
        unsigned v = 0;
        sscanf(s, "%2x", &v);
        out[n++] = static_cast<uint8_t>(v);
        s += 2;
    }
    return n;
}

}  // namespace

void test_2w_auth_matches_documented_capture(void) {
    // docs/linklayer.md, "2W Authentication" (and the push example):
    //   48 04 FEEFEE F00F00 36 9A02                 controller asks for the address
    //   0E 00 F00F00 FEEFEE 3C 123456789ABC 19DB    device challenges it
    //   0E 04 FEEFEE F00F00 3D C7FDC0668818 B1E3    controller answers
    // with the stack key 0102...1516 pushed in the same example. All three are
    // plain frames: no MAC trailer anywhere.
    const uint8_t STACK_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                   0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    uint8_t buf[iohome::FRAME_MAX_SIZE];

    iohome::frame::IoFrame cmd36, chal, answer;
    size_t n = unhex("4804FEEFEEF00F00369A02", buf);
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buf, n, &cmd36));
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&cmd36));
    n = unhex("0E00F00F00FEEFEE3C123456789ABC19DB", buf);
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buf, n, &chal));
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&chal));
    TEST_ASSERT_FALSE(chal.authenticated);
    TEST_ASSERT_EQUAL_UINT8(6, chal.data_len);
    n = unhex("0E04FEEFEEF00F003DC7FDC0668818B1E3", buf);
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buf, n, &answer));
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&answer));
    TEST_ASSERT_FALSE(answer.authenticated);

    // Controller side: having sent the 0x36, answer the device's 0x3C.
    mode2w::AuthenticationManager controller;
    controller.begin(STACK_KEY);
    TEST_ASSERT_TRUE(controller.note_request_sent(&cmd36, 1000));
    iohome::frame::IoFrame ours;
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &ours, 1100));
    TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_RESPONSE, ours.command_id);
    TEST_ASSERT_FALSE(ours.authenticated);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(answer.data, ours.data, 6);   // C7FDC0668818
    TEST_ASSERT_EQUAL_UINT8_ARRAY(answer.dest_node, ours.dest_node, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(answer.src_node, ours.src_node, 3);
    TEST_ASSERT_EQUAL_UINT(17, ours.frame_length);

    // Device side: the documented answer verifies against the 0x36 it received.
    // (The nonce is ours to choose in real life; pin it to the documented one.)
    struct PinnedNonce : mode2w::AuthenticationManager {
        void pin(const uint8_t n6[6]) { memcpy(current_challenge_, n6, 6); }
    } device;
    device.begin(STACK_KEY);
    const uint8_t device_addr[3] = {0xFE, 0xEF, 0xEE};
    iohome::frame::IoFrame our_chal;
    TEST_ASSERT_TRUE(device.create_challenge_for(&our_chal, &cmd36, device_addr, 1000));
    const uint8_t documented_nonce[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    device.pin(documented_nonce);
    TEST_ASSERT_TRUE(device.verify_challenge_response(&answer, 1100));
    iohome::frame::IoFrame verified;
    TEST_ASSERT_TRUE(device.take_verified_request(&verified));
    TEST_ASSERT_EQUAL_HEX8(0x36, verified.command_id);
    TEST_ASSERT_FALSE(device.take_verified_request(&verified));  // handed out once
}

void test_2w_push_key_answer_matches_documented_capture(void) {
    // The same rule on the push example's 0x32: 3D payload 8DC9D40DC7A4.
    const uint8_t STACK_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                   0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    uint8_t req[17];
    unhex("32F849584FFCFC442B1E97E4C38DF7B143", req);
    const uint8_t nonce[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    const uint8_t expected[6] = {0x8D, 0xC9, 0xD4, 0x0D, 0xC7, 0xA4};

    mode2w::AuthenticationManager m;
    m.begin(STACK_KEY);
    iohome::frame::IoFrame f;
    const uint8_t dev[3] = {0xFE, 0xEF, 0xEE}, ctrl[3] = {0xF0, 0x0F, 0x00};
    TEST_ASSERT_TRUE(m.create_challenge_response(&f, dev, ctrl, nonce, req, sizeof(req)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, f.data, 6);
}

void test_auth_handshake_succeeds(void) {
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);

    // Controller sends a plain command and remembers it.
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    TEST_ASSERT_FALSE(cmd.authenticated);
    TEST_ASSERT_TRUE(controller.note_request_sent(&cmd, 0));
    TEST_ASSERT_TRUE(controller.has_pending_request(0));

    // Actuator challenges it with a plain 0x3C.
    const iohome::frame::IoFrame cmd_rx = over_the_air(cmd);
    TEST_ASSERT_FALSE(cmd_rx.authenticated);
    iohome::frame::IoFrame chal;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd_rx, ACTUATOR, 0));
    TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_REQUEST, chal.command_id);
    TEST_ASSERT_FALSE(chal.authenticated);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(CONTROLLER, chal.dest_node, 3);

    iohome::frame::IoFrame answer;
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 50));
    TEST_ASSERT_FALSE(answer.authenticated);
    const iohome::frame::IoFrame answer_rx = over_the_air(answer);

    TEST_ASSERT_TRUE(actuator.verify_challenge_response(&answer_rx, 100));
    TEST_ASSERT_TRUE(actuator.is_authenticated_with(CONTROLLER, 100));
    iohome::frame::IoFrame verified;
    TEST_ASSERT_TRUE(actuator.take_verified_request(&verified));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(cmd.data, verified.data, cmd.data_len);
}

void test_auth_rejects_response_without_mac(void) {
    // Regression (finding 2): a 0x3D from the challenged peer with no MAC - or
    // any wrong payload - was accepted, because validate_frame() returned true
    // for a plain frame even when asked to verify a MAC.
    mode2w::AuthenticationManager actuator;
    actuator.begin(KEY);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    iohome::frame::IoFrame chal;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0));

    iohome::frame::IoFrame forged;
    iohome::frame::init_frame(&forged, false);
    iohome::frame::set_destination(&forged, ACTUATOR);
    iohome::frame::set_source(&forged, CONTROLLER);
    const uint8_t junk[6] = {0};
    iohome::frame::set_command(&forged, iohome::CMD_CHALLENGE_RESPONSE, junk, 6);
    iohome::frame::finalize_frame_plain(&forged);

    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&forged, 100));
    TEST_ASSERT_FALSE(actuator.is_authenticated(100));
    // ...and the attempt burned the nonce.
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, actuator.peek_state());
    iohome::frame::IoFrame out;
    TEST_ASSERT_FALSE(actuator.take_verified_request(&out));

    // An empty 0x3D is no better.
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 200));
    iohome::frame::set_command(&forged, iohome::CMD_CHALLENGE_RESPONSE, nullptr, 0);
    iohome::frame::finalize_frame_plain(&forged);
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&forged, 300));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, actuator.peek_state());
}

void test_validate_frame_with_key_rejects_plain_frames(void) {
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&cmd));          // CRC only
    TEST_ASSERT_FALSE(iohome::frame::validate_frame(&cmd, KEY));    // asked for a MAC
}

void test_answer_only_for_our_pending_request(void) {
    // Closing the chosen-nonce oracle: we never sign a nonce for a request we
    // did not send, nor for one we sent to somebody else.
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0));

    // Nothing pending: no answer.
    TEST_ASSERT_FALSE(controller.answer_challenge(&chal, &answer, 0));

    // Pending, but the challenge comes from another node.
    TEST_ASSERT_TRUE(controller.note_request_sent(&cmd, 0));
    const uint8_t STRANGER[3] = {0xEE, 0xEE, 0xEE};
    iohome::frame::IoFrame stray = chal;
    iohome::frame::set_source(&stray, STRANGER);
    TEST_ASSERT_FALSE(controller.answer_challenge(&stray, &answer, 10));
    // Or is addressed to another node.
    stray = chal;
    iohome::frame::set_destination(&stray, STRANGER);
    TEST_ASSERT_FALSE(controller.answer_challenge(&stray, &answer, 10));
    // Or arrives after the response window.
    controller.set_response_window_ms(500);
    TEST_ASSERT_FALSE(controller.answer_challenge(&chal, &answer, 501));
    TEST_ASSERT_FALSE(controller.has_pending_request(501));

    // A fresh request: answered a bounded number of times.
    TEST_ASSERT_TRUE(controller.note_request_sent(&cmd, 1000));
    for (uint8_t i = 0; i < mode2w::AuthenticationManager::MAX_ANSWERS_PER_REQUEST; i++) {
        TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 1000 + i));
    }
    TEST_ASSERT_FALSE(controller.answer_challenge(&chal, &answer, 1010));
}

void test_auth_rejects_replayed_response(void) {
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    controller.note_request_sent(&cmd, 0);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0));
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 10));
    TEST_ASSERT_TRUE(actuator.verify_challenge_response(&answer, 100));

    // Single-use: the same 0x3D again fails...
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&answer, 150));
    // ...and fails against a new challenge for the same command too, because
    // the new nonce is different.
    iohome::frame::IoFrame chal2;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal2, &cmd, ACTUATOR, 200));
    TEST_ASSERT_FALSE(memcmp(chal.data, chal2.data, 6) == 0);
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&answer, 250));
}

void test_answer_is_bound_to_the_request(void) {
    // A 0x3D proves the key for ONE command. An answer computed over a
    // different command (e.g. an attacker swapping OPEN for CLOSE) fails.
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    iohome::frame::IoFrame open_cmd = make_command(ACTUATOR, CONTROLLER, iohome::MP_OPEN);
    iohome::frame::IoFrame close_cmd = make_command(ACTUATOR, CONTROLLER, iohome::MP_CLOSE);

    controller.note_request_sent(&open_cmd, 0);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &close_cmd, ACTUATOR, 0));
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 10));
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&answer, 20));
}

void test_live_challenge_is_not_replaced(void) {
    mode2w::AuthenticationManager actuator;
    actuator.begin(KEY);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    iohome::frame::IoFrame chal;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0));
    TEST_ASSERT_FALSE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 100));
    // Once it times out a new one may start.
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR,
                     mode2w::AuthenticationManager::DEFAULT_CHALLENGE_TIMEOUT_MS + 1));
}

void test_stray_response_does_not_cancel_the_handshake(void) {
    // A 0x3D from a node we never challenged, or addressed to someone else,
    // must not touch our state - a neighbouring pair doing their own exchange
    // would otherwise cancel ours.
    const uint8_t STRANGER[3] = {0xEE, 0xEE, 0xEE};
    mode2w::AuthenticationManager controller, actuator, other;
    controller.begin(KEY);
    actuator.begin(KEY);
    other.begin(KEY);

    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    controller.note_request_sent(&cmd, 0);
    iohome::frame::IoFrame chal;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0));
    uint8_t nonce[6];
    memcpy(nonce, actuator.get_current_challenge(), 6);

    const uint8_t req[1] = {0x36};
    iohome::frame::IoFrame stray;
    TEST_ASSERT_TRUE(other.create_challenge_response(&stray, STRANGER, STRANGER, nonce, req, 1));
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&stray, 100));
    TEST_ASSERT_TRUE(actuator.has_active_challenge(100));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(nonce, actuator.get_current_challenge(), 6);

    iohome::frame::IoFrame misaddressed;
    TEST_ASSERT_TRUE(other.create_challenge_response(&misaddressed, STRANGER, CONTROLLER, nonce, req, 1));
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&misaddressed, 100));
    TEST_ASSERT_TRUE(actuator.has_active_challenge(100));

    iohome::frame::IoFrame answer;
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 100));
    TEST_ASSERT_TRUE(actuator.verify_challenge_response(&answer, 100));
}

void test_auth_rejects_wrong_key(void) {
    const uint8_t other_key[16] = {0x24};
    mode2w::AuthenticationManager impostor, actuator;
    impostor.begin(other_key);
    actuator.begin(KEY);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    impostor.note_request_sent(&cmd, 0);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0));
    TEST_ASSERT_TRUE(impostor.answer_challenge(&chal, &answer, 10));
    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&answer, 100));
    TEST_ASSERT_FALSE(actuator.is_authenticated(100));
    const uint8_t zero[6] = {0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, actuator.get_current_challenge(), 6);
}

void test_unbound_challenge_cannot_be_answered_into_success(void) {
    // A pairing 0x3C (create_challenge_request) authenticates no command, so no
    // 0x3D can verify against it.
    mode2w::AuthenticationManager device, controller;
    device.begin(KEY);
    controller.begin(KEY);
    iohome::frame::IoFrame chal;
    TEST_ASSERT_TRUE(device.create_challenge_request(&chal, CONTROLLER, ACTUATOR, 0));
    TEST_ASSERT_FALSE(chal.authenticated);
    const uint8_t req[1] = {0x31};
    iohome::frame::IoFrame answer;
    TEST_ASSERT_TRUE(controller.create_challenge_response(&answer, ACTUATOR, CONTROLLER,
                                                          chal.data, req, 1));
    TEST_ASSERT_FALSE(device.verify_challenge_response(&answer, 10));
}

void test_handshake_works_at_realistic_uptime(void) {
    // Regression: the challenge used to be timestamped at zero, so every
    // handshake more than challenge_timeout_ms_ after boot expired instantly.
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    const unsigned long uptime_ms = 86400000UL;

    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    controller.note_request_sent(&cmd, uptime_ms);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, uptime_ms));
    TEST_ASSERT_TRUE(actuator.has_active_challenge(uptime_ms));
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, uptime_ms + 100));
    TEST_ASSERT_TRUE(actuator.verify_challenge_response(&answer, uptime_ms + 200));
}

void test_auth_challenge_times_out(void) {
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    actuator.set_challenge_timeout_ms(5000);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    controller.note_request_sent(&cmd, 1000);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 1000));
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 1000));

    TEST_ASSERT_FALSE(actuator.verify_challenge_response(&answer, 1000 + 5001));
    TEST_ASSERT_EQUAL(mode2w::ChallengeState::IDLE, actuator.peek_state());
}

void test_auth_challenge_survives_up_to_the_timeout(void) {
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    actuator.set_challenge_timeout_ms(5000);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    controller.note_request_sent(&cmd, 1000);
    iohome::frame::IoFrame chal, answer;
    TEST_ASSERT_TRUE(actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 1000));
    TEST_ASSERT_TRUE(controller.answer_challenge(&chal, &answer, 1000));
    TEST_ASSERT_TRUE(actuator.verify_challenge_response(&answer, 1000 + 5000));
}

void test_auth_session_expires(void) {
    mode2w::AuthenticationManager controller, actuator;
    controller.begin(KEY);
    actuator.begin(KEY);
    actuator.set_session_timeout_ms(30000);
    iohome::frame::IoFrame cmd = make_command(ACTUATOR, CONTROLLER);
    controller.note_request_sent(&cmd, 0);
    iohome::frame::IoFrame chal, answer;
    actuator.create_challenge_for(&chal, &cmd, ACTUATOR, 0);
    controller.answer_challenge(&chal, &answer, 10);
    TEST_ASSERT_TRUE(actuator.verify_challenge_response(&answer, 1000));
    TEST_ASSERT_TRUE(actuator.is_authenticated(20000));
    TEST_ASSERT_FALSE(actuator.is_authenticated(40000));
}

void test_auth_rejects_nullptr(void) {
    mode2w::AuthenticationManager auth;
    const uint8_t node[3] = {1, 2, 3};
    auth.begin(KEY);
    const uint8_t nonce[6] = {0};
    const uint8_t req[1] = {0};

    iohome::frame::IoFrame frame;
    TEST_ASSERT_FALSE(auth.create_challenge_request(nullptr, node, node, 0));
    TEST_ASSERT_FALSE(auth.create_challenge_request(&frame, nullptr, node, 0));
    TEST_ASSERT_FALSE(auth.create_challenge_response(&frame, node, node, nullptr, req, 1));
    TEST_ASSERT_FALSE(auth.create_challenge_response(&frame, node, node, nonce, nullptr, 0));
    TEST_ASSERT_FALSE(auth.create_challenge_for(&frame, nullptr, node, 0));
    TEST_ASSERT_FALSE(auth.verify_challenge_response(nullptr, 0));
    TEST_ASSERT_FALSE(auth.note_request_sent(nullptr, 0));
    TEST_ASSERT_FALSE(auth.answer_challenge(nullptr, &frame, 0));
    TEST_ASSERT_FALSE(auth.take_verified_request(nullptr));
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
    RUN_TEST(test_2w_auth_matches_documented_capture);
    RUN_TEST(test_2w_push_key_answer_matches_documented_capture);
    RUN_TEST(test_auth_handshake_succeeds);
    RUN_TEST(test_auth_rejects_response_without_mac);
    RUN_TEST(test_validate_frame_with_key_rejects_plain_frames);
    RUN_TEST(test_answer_only_for_our_pending_request);
    RUN_TEST(test_auth_rejects_replayed_response);
    RUN_TEST(test_answer_is_bound_to_the_request);
    RUN_TEST(test_live_challenge_is_not_replaced);
    RUN_TEST(test_stray_response_does_not_cancel_the_handshake);
    RUN_TEST(test_auth_rejects_wrong_key);
    RUN_TEST(test_unbound_challenge_cannot_be_answered_into_success);
    RUN_TEST(test_handshake_works_at_realistic_uptime);
    RUN_TEST(test_auth_challenge_times_out);
    RUN_TEST(test_auth_challenge_survives_up_to_the_timeout);
    RUN_TEST(test_auth_session_expires);
    RUN_TEST(test_auth_rejects_nullptr);

    return UNITY_END();
}
